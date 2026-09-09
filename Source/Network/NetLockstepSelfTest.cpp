#include "NetLockstepSelfTest.h"

#include "LoopbackTransport.h"
#include "NetLockstep.h"
#include "NetProtocol.h"
#include "System/ScenarioRunner.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>

namespace RTE {

	namespace {
		ControllerFrame MakeFrame(int64_t actorId, uint64_t stateMask) {
			ControllerFrame frame;
			frame.actorUniqueID = actorId;
			frame.stateMask = stateMask;
			frame.analogMoveX = 123;
			frame.analogMoveY = -456;
			frame.analogAimX = 789;
			frame.analogAimY = -321;
			frame.inputMode = static_cast<uint8_t>(Controller::CIM_PLAYER);
			frame.playerRaw = Players::PlayerOne;
			frame.SetQuickDisabled(true);
			frame.aimAngle = 0.125F;
			frame.viewPointX = 12.5F;
			frame.viewPointY = -34.25F;
			frame.equippedFGUniqueID = 9001;
			frame.fgHandPosX = 1.5F;
			frame.fgHandPosY = 2.5F;
			return frame;
		}

		bool EncodePacket(const NetLockstepPacket& packet, std::vector<uint8_t>& bytes, std::string* error) {
			NetLockstepError encodeError;
			if (!NetLockstepCodec::Encode(packet, bytes, &encodeError)) {
				*error = "encode failed: " + encodeError.message;
				return false;
			}
			return true;
		}

		bool RoundTrip(const NetLockstepPacket& packet, std::string* error) {
			std::vector<uint8_t> bytes;
			if (!EncodePacket(packet, bytes, error)) {
				return false;
			}
			const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(bytes);
			if (!decoded.ok) {
				*error = "decode failed: " + decoded.error.message;
				return false;
			}
			if (!(decoded.packet == packet)) {
				*error = "decoded packet differed for " + std::string(NetLockstepCodec::PacketTypeName(NetLockstepCodec::PacketTypeOf(packet.payload)));
				return false;
			}
			return true;
		}

		bool ExpectDecodeError(const std::vector<uint8_t>& bytes, NetLockstepErrorCode code, std::string* error) {
			const NetLockstepDecodeResult result = NetLockstepCodec::Decode(bytes);
			if (result.ok) {
				*error = "expected decode failure for " + std::string(NetLockstepCodec::ErrorCodeName(code));
				return false;
			}
			if (result.error.code != code) {
				*error = "expected " + std::string(NetLockstepCodec::ErrorCodeName(code)) +
				         " got " + NetLockstepCodec::ErrorCodeName(result.error.code) +
				         ": " + result.error.message;
				return false;
			}
			return true;
		}

		NetSoundObservation MakeObservation(uint8_t sender, uint64_t objectUID, uint64_t tick, uint64_t phase, uint64_t ordinal, float value) {
			NetSoundObservation observation;
			observation.senderPeerId = sender;
			observation.objectUID = objectUID;
			observation.tick = tick;
			observation.phase = phase;
			observation.occurrence = 0;
			observation.ordinal = ordinal;
			observation.value = value;
			return observation;
		}

		// The shape a battle actually produces: one live sound per object, its key fixed for the sound's
		// life and its reading moving every tick. Keys are spread over a realistic UID range and share the
		// handful of script-hook hashes a phase comes from.
		std::vector<NetSoundObservation> MakeObservationSet(uint8_t sender, size_t count, uint64_t startTick, float bias) {
			static const uint64_t phases[4] = {0x9E3779B97F4A7C15ULL, 0xC2B2AE3D27D4EB4FULL, 0x165667B19E3779F9ULL, 0x27D4EB2F165667C5ULL};
			std::vector<NetSoundObservation> observations;
			observations.reserve(count);
			for (size_t i = 0; i < count; ++i) {
				observations.push_back(MakeObservation(sender, 1048576 + static_cast<uint64_t>(i) * 3, startTick + i % 7, phases[i % 4], 1 + i % 5,
				                                       0.25F + static_cast<float>((i + static_cast<size_t>(bias * 64.0F)) % 64) / 256.0F));
			}
			return observations;
		}

		size_t FrameBytesWithoutObservations(const NetLockstepFrame& frame) {
			NetLockstepFrame stripped = frame;
			stripped.observations.clear();
			std::vector<uint8_t> bytes;
			return NetLockstepCodec::Encode({stripped}, bytes) ? bytes.size() : 0;
		}

		// The same frame as a version 13 packet: identical up to the observations, which spell out five
		// full-width key fields and a reading each. Everything before the block is byte for byte what the
		// current encoder writes, so this is a real recording's shape and not a guess at one.
		std::vector<uint8_t> MakeVersion13Frame(const NetLockstepFrame& frame) {
			NetLockstepFrame stripped = frame;
			stripped.observations.clear();
			std::vector<uint8_t> bytes;
			if (!NetLockstepCodec::Encode({stripped}, bytes)) {
				return {};
			}
			bytes.resize(bytes.size() - 3); // The empty observation block: a zero binding sequence and a zero count.
			const auto appendU16 = [&bytes](uint16_t value) { bytes.push_back(static_cast<uint8_t>(value)); bytes.push_back(static_cast<uint8_t>(value >> 8)); };
			const auto appendU32 = [&bytes](uint32_t value) { for (int i = 0; i < 4; ++i) { bytes.push_back(static_cast<uint8_t>(value >> (i * 8))); } };
			const auto appendU64 = [&bytes](uint64_t value) { for (int i = 0; i < 8; ++i) { bytes.push_back(static_cast<uint8_t>(value >> (i * 8))); } };
			appendU16(static_cast<uint16_t>(frame.observations.size()));
			for (const NetSoundObservation& observation : frame.observations) {
				appendU64(observation.objectUID);
				appendU64(observation.tick);
				appendU64(observation.phase);
				appendU64(observation.occurrence);
				appendU64(observation.ordinal);
				uint32_t valueBits = 0;
				std::memcpy(&valueBits, &observation.value, sizeof(valueBits));
				appendU32(valueBits);
			}
			bytes[4] = 13;
			bytes[5] = 0;
			const uint32_t payloadLength = static_cast<uint32_t>(bytes.size() - NetLockstepCodec::c_HeaderBytes);
			for (int i = 0; i < 4; ++i) {
				bytes[12 + i] = static_cast<uint8_t>(payloadLength >> (i * 8));
			}
			return bytes;
		}

		NetLockstepFrame MakeObservationFrame(uint64_t targetFrame, uint64_t roundId, std::vector<NetSoundObservation> observations) {
			NetLockstepFrame frame;
			frame.senderPeerId = 2;
			frame.targetFrame = targetFrame;
			frame.roundId = roundId;
			frame.frames = {MakeFrame(100, 1)};
			frame.observations = std::move(observations);
			return frame;
		}

		bool TestObservationSlotCodec(std::string* error) {
			const uint64_t roundId = 0x5EED0000C0FFEE14ULL;
			NetSoundObservationDictionary encoder;
			NetSoundObservationTables decoderTables;

			const auto roundTripThrough = [&](const NetLockstepFrame& frame, size_t& outBytes, size_t& outEncoded) {
				std::vector<uint8_t> bytes;
				NetLockstepError encodeError;
				outEncoded = frame.observations.size();
				if (!NetLockstepCodec::Encode({frame}, bytes, &encodeError, &encoder, &outEncoded)) {
					*error = "compact observation encode failed: " + encodeError.message;
					return false;
				}
				outBytes = bytes.size();
				const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(bytes, ControllerFrame::c_Version, &decoderTables);
				if (!decoded.ok) {
					*error = "compact observation decode failed: " + decoded.error.message;
					return false;
				}
				const NetLockstepFrame* out = std::get_if<NetLockstepFrame>(&decoded.packet.payload);
				if (!out || out->observations.size() != outEncoded) {
					*error = "compact observation decode returned the wrong count";
					return false;
				}
				for (size_t i = 0; i < outEncoded; ++i) {
					if (!(out->observations[i] == frame.observations[i])) {
						*error = "compact observation " + std::to_string(i) + " did not survive the wire";
						return false;
					}
				}
				return true;
			};

			// First use spells the key out; the same keys next frame are slot references only.
			const std::vector<NetSoundObservation> firstSet = MakeObservationSet(2, 64, 400, 0.0F);
			const NetLockstepFrame first = MakeObservationFrame(10, roundId, firstSet);
			const size_t emptyBytes = FrameBytesWithoutObservations(first);
			size_t firstBytes = 0;
			size_t firstEncoded = 0;
			if (!roundTripThrough(first, firstBytes, firstEncoded) || firstEncoded != 64) {
				if (error->empty()) { *error = "first observation frame did not encode every observation"; }
				return false;
			}
			std::vector<NetSoundObservation> repeatSet = firstSet;
			for (NetSoundObservation& observation : repeatSet) {
				observation.value += 0.001953125F; // A changed reading of the same sound.
			}
			size_t repeatBytes = 0;
			size_t repeatEncoded = 0;
			if (!roundTripThrough(MakeObservationFrame(11, roundId, repeatSet), repeatBytes, repeatEncoded) || repeatEncoded != 64) {
				if (error->empty()) { *error = "repeat observation frame did not encode every observation"; }
				return false;
			}
			// Five bytes each: a one-byte slot reference and the reading, after the two-byte count.
			if (repeatBytes - emptyBytes != 5 * 64) {
				*error = "a repeated observation did not cost five bytes: block=" + std::to_string(repeatBytes - emptyBytes);
				return false;
			}
			if (firstBytes <= repeatBytes || firstBytes - emptyBytes > 24 * 64) {
				*error = "a first-use observation block was not in its expected range: " + std::to_string(firstBytes - emptyBytes);
				return false;
			}

			// Filling the table evicts the key nobody has mentioned since; it returns spelled out in full.
			const NetSoundObservationKey evicted = KeyOfObservation(firstSet.front());
			uint64_t nextFrame = 12;
			for (size_t block = 0; block * 256 < NetSoundObservationDictionary::c_MaxSlots; ++block) {
				std::vector<NetSoundObservation> fresh;
				for (size_t i = 0; i < 256; ++i) {
					fresh.push_back(MakeObservation(2, 9000000 + block * 256 + i, 500 + i, 0x1234ULL + i, 1, 0.5F));
				}
				size_t bytes = 0;
				size_t encoded = 0;
				if (!roundTripThrough(MakeObservationFrame(nextFrame++, roundId, fresh), bytes, encoded) || encoded != fresh.size()) {
					if (error->empty()) { *error = "a slot-exhaustion frame did not encode every observation"; }
					return false;
				}
			}
			uint16_t stillBound = 0;
			if (encoder.Lookup(evicted, stillBound)) {
				*error = "the least recently used key was not evicted once every slot was bound";
				return false;
			}
			size_t returnBytes = 0;
			size_t returnEncoded = 0;
			if (!roundTripThrough(MakeObservationFrame(nextFrame++, roundId, {firstSet.front()}), returnBytes, returnEncoded) || returnEncoded != 1) {
				if (error->empty()) { *error = "a returning key did not encode"; }
				return false;
			}
			if (returnBytes - emptyBytes <= 5) {
				*error = "a returning key was sent as a bare slot reference";
				return false;
			}

			// More first-use observations than the byte budget holds: the encoder stops, says how many it
			// took, and the rest encode next frame against a table that never saw them.
			std::vector<NetSoundObservation> flood;
			for (size_t i = 0; i < NetLockstepCodec::c_MaxObservationsPerPacket; ++i) {
				flood.push_back(MakeObservation(2, 20000000 + i * 7, 900 + i, 0xABCDEF0123456789ULL + i, 1 + i % 3, 0.75F));
			}
			size_t floodBytes = 0;
			size_t floodEncoded = 0;
			if (!roundTripThrough(MakeObservationFrame(nextFrame++, roundId, flood), floodBytes, floodEncoded)) {
				return false;
			}
			if (floodEncoded == 0 || floodEncoded >= flood.size()) {
				*error = "the observation byte budget did not stop a flood of new keys: encoded " + std::to_string(floodEncoded);
				return false;
			}
			// The slack is the block's own binding sequence, which is wider here than in an empty block.
			if (floodBytes - emptyBytes > NetLockstepCodec::c_MaxObservationBytesPerPacket + 16 ||
			    floodBytes > NetLockstepCodec::c_HeaderBytes + NetLockstepCodec::c_MaxPayloadBytes) {
				*error = "an observation block passed its byte budget";
				return false;
			}
			const std::vector<NetSoundObservation> carried(flood.begin() + static_cast<std::ptrdiff_t>(floodEncoded), flood.end());
			size_t carriedBytes = 0;
			size_t carriedEncoded = 0;
			if (!roundTripThrough(MakeObservationFrame(nextFrame++, roundId, carried), carriedBytes, carriedEncoded)) {
				return false;
			}
			if (carriedEncoded == 0) {
				*error = "the carried remainder encoded nothing";
				return false;
			}

			// A version 13 recording still decodes, with and without a table to read slots against.
			const NetLockstepFrame legacy = MakeObservationFrame(40, roundId, MakeObservationSet(2, 5, 77, 0.5F));
			const std::vector<uint8_t> legacyBytes = MakeVersion13Frame(legacy);
			// One byte less than an empty version 15 block, because version 13 has no binding sequence.
			if (legacyBytes.size() != FrameBytesWithoutObservations(legacy) - 1 + 44 * 5) {
				*error = "the version 13 frame is not forty-four bytes per observation";
				return false;
			}
			for (NetSoundObservationTables* tables: {static_cast<NetSoundObservationTables*>(nullptr), &decoderTables}) {
				const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(legacyBytes, ControllerFrame::c_Version, tables);
				if (!decoded.ok) {
					*error = "a version 13 frame did not decode: " + decoded.error.message;
					return false;
				}
				const NetLockstepFrame* out = std::get_if<NetLockstepFrame>(&decoded.packet.payload);
				if (!out || out->observations != legacy.observations) {
					*error = "a version 13 frame decoded to different observations";
					return false;
				}
			}

			// A slot reference nobody spelled out is refused, whether the table is missing or just lacks it.
			NetSoundObservationDictionary lone;
			std::vector<uint8_t> bindBytes;
			size_t loneEncoded = 0;
			if (!NetLockstepCodec::Encode({MakeObservationFrame(50, 0, MakeObservationSet(2, 2, 3, 0.0F))}, bindBytes, nullptr, &lone, &loneEncoded)) {
				*error = "could not encode the binding frame";
				return false;
			}
			NetSoundObservationTables mirror;
			if (!NetLockstepCodec::Decode(bindBytes, ControllerFrame::c_Version, &mirror).ok) {
				*error = "could not decode the binding frame";
				return false;
			}
			std::vector<uint8_t> refBytes;
			size_t refEncoded = 0;
			if (!NetLockstepCodec::Encode({MakeObservationFrame(51, 0, MakeObservationSet(2, 2, 3, 0.25F))}, refBytes, nullptr, &lone, &refEncoded)) {
				*error = "could not encode the slot-reference frame";
				return false;
			}
			if (NetLockstepCodec::Decode(refBytes, ControllerFrame::c_Version, nullptr).error.code != NetLockstepErrorCode::UnboundObservationSlot) {
				*error = "a slot reference without any table was not refused";
				return false;
			}
			// A table that has never been fed sees the gap before it ever reaches the slot.
			NetSoundObservationTables emptyTables;
			if (NetLockstepCodec::Decode(refBytes, ControllerFrame::c_Version, &emptyTables).error.code != NetLockstepErrorCode::ObservationBindingGap) {
				*error = "a slot reference against an empty table was not refused";
				return false;
			}
			if (!NetLockstepCodec::Decode(refBytes, ControllerFrame::c_Version, &mirror).ok) {
				*error = "a slot reference did not decode against the table that bound it";
				return false;
			}

			// The one packet that says a slot has been reused is the only place that is ever said, so a
			// receiver that misses it must refuse rather than read the slot as the key it held before.
			// This is the reviewer's lost_rebinding probe: it decodes with no error on version 14.
			uint64_t silentlyWrongKey = 0;
			{
				NetSoundObservationDictionary sender;
				NetSoundObservationTables receiver;
				uint64_t nextFrame = 100;
				// Bind every slot, so the next key handed out reuses the one nobody has mentioned since.
				for (size_t block = 0; block * 512 < NetSoundObservationDictionary::c_MaxSlots; ++block) {
					std::vector<NetSoundObservation> keys;
					for (size_t i = 0; i < 512; ++i) {
						keys.push_back(MakeObservation(2, 700000 + block * 512 + i, 11, 0x5151ULL + i, 1, 0.125F));
					}
					std::vector<uint8_t> bytes;
					size_t encoded = 0;
					if (!NetLockstepCodec::Encode({MakeObservationFrame(nextFrame++, roundId, keys)}, bytes, nullptr, &sender, &encoded) || encoded != keys.size()) {
						*error = "the rebinding probe could not fill the table";
						return false;
					}
					if (!NetLockstepCodec::Decode(bytes, ControllerFrame::c_Version, &receiver).ok) {
						*error = "the rebinding probe's filling frames did not decode";
						return false;
					}
				}
				const NetSoundObservation reused = MakeObservation(2, 990001, 12, 0x6262ULL, 1, 0.25F);
				std::vector<uint8_t> rebinding;
				size_t rebindingEncoded = 0;
				if (!NetLockstepCodec::Encode({MakeObservationFrame(nextFrame++, roundId, {reused})}, rebinding, nullptr, &sender, &rebindingEncoded)) {
					*error = "the rebinding frame did not encode";
					return false;
				}
				// The receiver never gets that frame. The next one refers to the reused slot.
				NetSoundObservation later = reused;
				later.value = 0.5F;
				std::vector<uint8_t> after;
				size_t afterEncoded = 0;
				if (!NetLockstepCodec::Encode({MakeObservationFrame(nextFrame++, roundId, {later})}, after, nullptr, &sender, &afterEncoded)) {
					*error = "the frame after the rebinding did not encode";
					return false;
				}
				const NetLockstepDecodeResult lost = NetLockstepCodec::Decode(after, ControllerFrame::c_Version, &receiver);
				if (lost.ok || lost.error.code != NetLockstepErrorCode::ObservationBindingGap) {
					*error = "a lost rebinding was not refused: ok=" + std::to_string(lost.ok ? 1 : 0) + " " + NetLockstepCodec::ErrorCodeName(lost.error.code);
					return false;
				}
				// The control: the same packet as version 14 said nothing about the bindings behind it, so the
				// same receiver reads the reused slot as the key it held before and commits the sent reading
				// against the wrong sound, with no error at all. That is what the sequence above refuses.
				const size_t prefix = FrameBytesWithoutObservations(MakeObservationFrame(0, roundId, {})) - 3;
				std::vector<uint8_t> asVersion14 = after;
				size_t sequenceBytes = 0;
				while (prefix + sequenceBytes < asVersion14.size() && (asVersion14[prefix + sequenceBytes] & 0x80U) != 0) {
					++sequenceBytes;
				}
				++sequenceBytes;
				asVersion14.erase(asVersion14.begin() + static_cast<std::ptrdiff_t>(prefix),
				                  asVersion14.begin() + static_cast<std::ptrdiff_t>(prefix + sequenceBytes));
				asVersion14[4] = 14;
				for (int i = 0; i < 4; ++i) {
					asVersion14[12 + i] = static_cast<uint8_t>((asVersion14.size() - NetLockstepCodec::c_HeaderBytes) >> (i * 8));
				}
				const NetLockstepDecodeResult silent = NetLockstepCodec::Decode(asVersion14, ControllerFrame::c_Version, &receiver);
				const NetLockstepFrame* silentFrame = silent.ok ? std::get_if<NetLockstepFrame>(&silent.packet.payload) : nullptr;
				if (!silentFrame || silentFrame->observations.size() != 1) {
					*error = "the version 14 control did not decode, so it proves nothing";
					return false;
				}
				if (silentFrame->observations.front().objectUID == later.objectUID) {
					*error = "the version 14 control did not reproduce the wrong key it is there to show";
					return false;
				}
				silentlyWrongKey = silentFrame->observations.front().objectUID;
				// Delivered in order, the same two frames read exactly what the sender meant.
				NetSoundObservationDictionary replaySender;
				NetSoundObservationTables replayReceiver;
				uint64_t replayFrame = 200;
				bool replayOk = true;
				for (size_t block = 0; block * 512 < NetSoundObservationDictionary::c_MaxSlots && replayOk; ++block) {
					std::vector<NetSoundObservation> keys;
					for (size_t i = 0; i < 512; ++i) {
						keys.push_back(MakeObservation(2, 700000 + block * 512 + i, 11, 0x5151ULL + i, 1, 0.125F));
					}
					std::vector<uint8_t> bytes;
					size_t encoded = 0;
					replayOk = NetLockstepCodec::Encode({MakeObservationFrame(replayFrame++, roundId, keys)}, bytes, nullptr, &replaySender, &encoded) &&
					           NetLockstepCodec::Decode(bytes, ControllerFrame::c_Version, &replayReceiver).ok;
				}
				std::vector<uint8_t> replayRebinding, replayAfter;
				size_t ignored = 0;
				replayOk = replayOk &&
				           NetLockstepCodec::Encode({MakeObservationFrame(replayFrame++, roundId, {reused})}, replayRebinding, nullptr, &replaySender, &ignored) &&
				           NetLockstepCodec::Decode(replayRebinding, ControllerFrame::c_Version, &replayReceiver).ok &&
				           NetLockstepCodec::Encode({MakeObservationFrame(replayFrame++, roundId, {later})}, replayAfter, nullptr, &replaySender, &ignored);
				if (!replayOk) {
					*error = "the in-order replay of the rebinding stream failed";
					return false;
				}
				const NetLockstepDecodeResult delivered = NetLockstepCodec::Decode(replayAfter, ControllerFrame::c_Version, &replayReceiver);
				const NetLockstepFrame* deliveredFrame = delivered.ok ? std::get_if<NetLockstepFrame>(&delivered.packet.payload) : nullptr;
				if (!deliveredFrame || deliveredFrame->observations.size() != 1 || !(deliveredFrame->observations.front() == later)) {
					*error = "the reused slot did not read as the key the sender rebound it to";
					return false;
				}
			}

			// A sender that starts its round over has spelled nothing out yet; that is a fresh table, not a
			// hole, and the keys it sends next are its own.
			{
				NetSoundObservationDictionary warm;
				NetSoundObservationTables receiver;
				std::vector<uint8_t> bytes;
				size_t encoded = 0;
				if (!NetLockstepCodec::Encode({MakeObservationFrame(300, roundId, MakeObservationSet(2, 8, 21, 0.0F))}, bytes, nullptr, &warm, &encoded) ||
				    !NetLockstepCodec::Decode(bytes, ControllerFrame::c_Version, &receiver).ok) {
					*error = "the restart probe's first round did not survive the wire";
					return false;
				}
				NetSoundObservationDictionary restarted;
				const std::vector<NetSoundObservation> fresh = MakeObservationSet(2, 8, 44, 0.5F);
				std::vector<uint8_t> restartedBytes;
				if (!NetLockstepCodec::Encode({MakeObservationFrame(0, roundId + 1, fresh)}, restartedBytes, nullptr, &restarted, &encoded)) {
					*error = "the restarted round did not encode";
					return false;
				}
				const NetLockstepDecodeResult adopted = NetLockstepCodec::Decode(restartedBytes, ControllerFrame::c_Version, &receiver);
				const NetLockstepFrame* adoptedFrame = adopted.ok ? std::get_if<NetLockstepFrame>(&adopted.packet.payload) : nullptr;
				if (!adoptedFrame || adoptedFrame->observations != fresh) {
					*error = "a sender that started over was not followed: " + std::string(NetLockstepCodec::ErrorCodeName(adopted.error.code));
					return false;
				}
			}

			// Corruptions inside the block are refused, not read as another sound.
			NetSoundObservationDictionary roundEncoder;
			std::vector<uint8_t> roundBytes;
			size_t roundEncoded = 0;
			if (!NetLockstepCodec::Encode({MakeObservationFrame(60, roundId, MakeObservationSet(2, 3, 9, 0.0F))}, roundBytes, nullptr, &roundEncoder, &roundEncoded)) {
				*error = "could not encode the corruption frame";
				return false;
			}
			// Past the frame's empty block, which is a zero binding sequence and a zero count.
			const size_t blockStart = FrameBytesWithoutObservations(MakeObservationFrame(60, roundId, {}));
			const auto expectBlockError = [&](size_t offset, uint8_t value, NetLockstepErrorCode code, const char* what) {
				std::vector<uint8_t> corrupt = roundBytes;
				corrupt[offset] = value;
				NetSoundObservationTables tables;
				const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(corrupt, ControllerFrame::c_Version, &tables);
				if (decoded.error.code == code) {
					return true;
				}
				*error = std::string(what) + " gave " + NetLockstepCodec::ErrorCodeName(decoded.error.code);
				return false;
			};
			if (!expectBlockError(blockStart - 3, 0x05U, NetLockstepErrorCode::ObservationBindingGap, "a binding sequence ahead of the table") ||
			    !expectBlockError(blockStart, 0xFEU, NetLockstepErrorCode::UnboundObservationSlot, "a corrupted slot reference") ||
			    !expectBlockError(blockStart + 1, 0xE0U, NetLockstepErrorCode::ReservedFieldNonZero, "a reserved key-mask bit")) {
				return false;
			}
			std::vector<uint8_t> nonCanonical = roundBytes;
			nonCanonical[blockStart] = 0x81U; // A varint continuation whose next byte adds nothing.
			nonCanonical.insert(nonCanonical.begin() + static_cast<std::ptrdiff_t>(blockStart) + 1, 0x00U);
			for (int i = 0; i < 4; ++i) {
				nonCanonical[12 + i] = static_cast<uint8_t>((nonCanonical.size() - NetLockstepCodec::c_HeaderBytes) >> (i * 8));
			}
			NetSoundObservationTables varintTables;
			if (NetLockstepCodec::Decode(nonCanonical, ControllerFrame::c_Version, &varintTables).error.code != NetLockstepErrorCode::InvalidValue) {
				*error = "a non-canonical slot varint was not refused";
				return false;
			}

			// A version 14 frame has no binding sequence and still decodes, with and without a table.
			std::vector<uint8_t> version14 = roundBytes;
			if (version14[blockStart - 3] != 0x00U) {
				*error = "the corruption frame's binding sequence is not the single zero byte expected";
				return false;
			}
			version14.erase(version14.begin() + static_cast<std::ptrdiff_t>(blockStart) - 3);
			version14[4] = 14;
			for (int i = 0; i < 4; ++i) {
				version14[12 + i] = static_cast<uint8_t>((version14.size() - NetLockstepCodec::c_HeaderBytes) >> (i * 8));
			}
			for (NetSoundObservationTables* tables: {static_cast<NetSoundObservationTables*>(nullptr), &varintTables}) {
				const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(version14, ControllerFrame::c_Version, tables);
				const NetLockstepFrame* out = decoded.ok ? std::get_if<NetLockstepFrame>(&decoded.packet.payload) : nullptr;
				if (!out || out->observations != MakeObservationSet(2, 3, 9, 0.0F)) {
					*error = "a version 14 frame did not decode: " + std::string(NetLockstepCodec::ErrorCodeName(decoded.error.code));
					return false;
				}
			}
			std::cout << "[net-lockstep-selftest] PASS observation_slot_codec repeat=" << (repeatBytes - emptyBytes) / 64
			          << "B first_use=" << (firstBytes - emptyBytes) / 64 << "B legacy=44B budget_stop=" << floodEncoded << "/" << flood.size()
			          << " lost_rebinding=refused v14_control_committed_key=" << silentlyWrongKey << std::endl;
			return true;
		}

		bool TestRoundTrips(std::string* error) {
			const NetLockstepStart start{
				0xAABBCCDDEEFF0011ULL,
				30,
				2,
				ControllerFrame::c_Version,
				static_cast<uint16_t>(ControllerFrame::c_EncodedSize),
				1,
				2,
				"SimBaseline",
				"unique-id-split",
				0x5EED0000C0FFEE01ULL,
			};
			if (!RoundTrip({start}, error)) {
				return false;
			}

			NetLockstepFrame frame;
			frame.senderPeerId = 2;
			frame.targetFrame = 32;
			frame.roundId = 0x5EED0000C0FFEE01ULL;
			frame.observations = {NetSoundObservation{2, 1048601, 31, 0x1122334455667788ULL, 7, 3, 0.25F}, NetSoundObservation{2, 0, 12, 99, 0, 1, 0.75F}};
			frame.frames = {MakeFrame(100, 1), MakeFrame(200, 2)};
			frame.commands = {NetGameCommand{2, NetGameSetTeamFunds{0, 1500}}, NetGameCommand{2, NetGameSetTeamFunds{1, -250}}, NetGameCommand{2, NetGameSpawnActor{"AHuman", "Green Dummy", "Base.rte", 1234.5F, -67.25F, 1}}, NetGameCommand{2, NetGameDeliverCargo{"ACDropShip", "Dropship MK1", "Base.rte", 880.0F, 48.5F, 0, {{"AHuman", "Green Dummy", "Base.rte"}, {"AHuman", "Robot 1", "Base.rte"}}}}, NetGameCommand{2, NetGameDeliverCargo{"ACRocket", "Rocket MK2", "Base.rte", 512.0F, 300.0F, 1, {{"AHuman", "Green Dummy", "Base.rte"}}, true, 137.5F, false, 4, 600.0F, 350.25F, 424242, 1, -32.0F}}, NetGameCommand{2, NetGameScuttleCraft{17143, 0}}, NetGameCommand{2, NetGameInventoryOp{9001, 1, NetGameInventoryOp::Drop, 0, 2, true, 0.5F, -0.25F}}, NetGameCommand{2, NetGamePauseMatch{1, true}}, NetGameCommand{2, NetGamePauseMatch{0, false}}, NetGameCommand{2, NetGameSetActorAIMode{31337, 1, 6}}, NetGameCommand{2, NetGameSwitchControl{41414, 0, 2}}, NetGameCommand{2, NetGameAIEquip{51515, 1, NetGameAIEquip::LoadedFirearmInGroup, false, "Weapons - Primary", "Weapons - Explosive", "", ""}}, NetGameCommand{2, NetGameAIEquip{51516, 0, NetGameAIEquip::NamedDevice, false, "", "", "Base.rte", "Battle Rifle"}}, NetGameCommand{2, NetGameAIEquip{51517, 1, NetGameAIEquip::ShieldInBGArm, true, "", "", "", ""}}, NetGameCommand{2, NetGameAIEquip{51518, 0, NetGameAIEquip::UnequipFGArm, false, "", "", "", ""}}, NetGameCommand{2, NetGameAIOrder{61616, 0, NetGameAIOrder::FormSquad, 512.5F, -12.25F, 61617}}, NetGameCommand{2, NetGameAIOrder{61618, 1, NetGameAIOrder::MOWaypoint, 0.0F, 0.0F, 61616}}, NetGameCommand{2, NetGameSoundOp{71717, 1, 0x00FF00FF00FF0001ULL, NetGameSoundOp::Play, 0, 3, 0, 0.0F, 0.0F, {}, ""}}, NetGameCommand{2, NetGameSoundOp{71718, 0, 0x0000000000000002ULL, NetGameSoundOp::SetProperty, 13, -1, 0, -12.5F, 88.25F, {}, ""}}, NetGameCommand{2, NetGameSoundOp{71719, 1, 0x0000000000000003ULL, NetGameSoundOp::SelectSounds, 0, -1, 0, 0.0F, 0.0F, {2, 0, 7}, ""}}, NetGameCommand{2, NetGameSoundOp{71720, 0, 0x0000000000000004ULL, NetGameSoundOp::FadeOut, 0, -1, 250, 0.0F, 0.0F, {}, ""}}, NetGameCommand{2, NetGameSoundOp{71721, 1, 0x0000000000000005ULL, NetGameSoundOp::AddSound, 0, -1, 0, 0.0F, 0.0F, {1}, "9 SoundData1 31 Base.rte/Sounds/GUIs/Click.flac 0 0 0 3212836864 "}}, NetGameCommand{2, NetGameSoundOp{71722, 0, 0x0000000000000006ULL, NetGameSoundOp::SetCycleMode, 0, -1, 2, 0.0F, 0.0F, {}, ""}}};
			if (!RoundTrip({frame}, error)) {
				return false;
			}

			if (!RoundTrip({NetLockstepAck{1, 31, 0x0000FFFFU}}, error)) {
				return false;
			}
			if (!RoundTrip({NetLockstepStop{2, NetLockstepStopReason::Complete, 120, "done"}}, error)) {
				return false;
			}
			if (!RoundTrip({NetLockstepStop{3, NetLockstepStopReason::PeerLeft, 240, "left"}}, error)) {
				return false;
			}
			if (!RoundTrip({NetLockstepStop{1, NetLockstepStopReason::ResyncRequested, 300, "rejoin"}}, error)) {
				return false;
			}
			std::array<uint8_t, 32> checksumHash{};
			for (size_t i = 0; i < checksumHash.size(); ++i) {
				checksumHash[i] = static_cast<uint8_t>(i * 7 + 3);
			}
			if (!RoundTrip({NetLockstepChecksum{1, 99, checksumHash, 0x5EED0000C0FFEE01ULL}}, error)) {
				return false;
			}
			return true;
		}

		bool TestCanonicalHeader(std::string* error) {
			std::vector<uint8_t> bytes;
			if (!EncodePacket({NetLockstepAck{1, 0x1122334455667788ULL, 0xAABBCCDDU}}, bytes, error)) {
				return false;
			}
			if (bytes.size() != NetLockstepCodec::c_HeaderBytes + 16U) {
				*error = "canonical ack encoded size mismatch";
				return false;
			}
			const std::vector<uint8_t> expectedPrefix = {
				0x43, 0x43, 0x4C, 0x33,
				0x0F, 0x00,
				0x10, 0x00,
				0x03, 0x00,
				0x00, 0x00,
				0x10, 0x00, 0x00, 0x00,
				0x01, 0x00, 0x00, 0x00,
			};
			for (size_t i = 0; i < expectedPrefix.size(); ++i) {
				if (bytes[i] != expectedPrefix[i]) {
					*error = "canonical header byte mismatch at " + std::to_string(i);
					return false;
				}
			}
			if (bytes[20] != 0x88U || bytes[27] != 0x11U || bytes[28] != 0xDDU || bytes[31] != 0xAAU) {
				*error = "canonical ack payload is not little-endian";
				return false;
			}
			return true;
		}

		bool TestDecodeFailures(std::string* error) {
			std::vector<uint8_t> bytes;
			if (!EncodePacket({NetLockstepAck{1, 2, 3}}, bytes, error)) {
				return false;
			}

			if (!ExpectDecodeError({}, NetLockstepErrorCode::ShortHeader, error)) {
				return false;
			}

			std::vector<uint8_t> mutated = bytes;
			mutated[0] = 0;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::BadMagic, error)) {
				return false;
			}

			mutated = bytes;
			mutated[4] = 0xFFU;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::UnsupportedVersion, error)) {
				return false;
			}

			mutated = bytes;
			mutated[6] = 0x18U;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::BadHeaderSize, error)) {
				return false;
			}

			mutated = bytes;
			mutated[8] = 0xFEU;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::UnknownPacketType, error)) {
				return false;
			}

			mutated = bytes;
			mutated[10] = 0x01U;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::UnknownFlags, error)) {
				return false;
			}

			mutated = bytes;
			mutated[12] = 0xFFU;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::PayloadLengthMismatch, error)) {
				return false;
			}

			mutated = bytes;
			mutated[12] = 0x01U;
			mutated[14] = 0x01U;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::PayloadTooLarge, error)) {
				return false;
			}

			mutated = bytes;
			mutated.push_back(0);
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::PayloadLengthMismatch, error)) {
				return false;
			}

			mutated = bytes;
			mutated.resize(NetLockstepCodec::c_HeaderBytes + 4U);
			mutated[12] = 0x04U;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::TruncatedPayload, error)) {
				return false;
			}

			mutated = bytes;
			mutated[NetLockstepCodec::c_HeaderBytes + 1] = 1U;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::ReservedFieldNonZero, error)) {
				return false;
			}
			return true;
		}

		bool TestSemanticFailures(std::string* error) {
			NetLockstepStart start{
				1,
				0,
				2,
				ControllerFrame::c_Version,
				static_cast<uint16_t>(ControllerFrame::c_EncodedSize),
				1,
				2,
				"bad\nscenario",
				"unique-id-split",
			};
			std::vector<uint8_t> bytes;
			NetLockstepError encodeError;
			if (NetLockstepCodec::Encode({start}, bytes, &encodeError) || encodeError.code != NetLockstepErrorCode::InvalidString) {
				*error = "expected invalid start string encode failure";
				return false;
			}

			start.scenario.assign(NetLockstepCodec::c_MaxScenarioBytes + 1, 'x');
			if (NetLockstepCodec::Encode({start}, bytes, &encodeError) || encodeError.code != NetLockstepErrorCode::StringTooLong) {
				*error = "expected oversized start string encode failure";
				return false;
			}

			start.scenario = "SimBaseline";
			start.controllerFrameVersion = ControllerFrame::c_Version + 1;
			if (NetLockstepCodec::Encode({start}, bytes, &encodeError) || encodeError.code != NetLockstepErrorCode::InvalidValue) {
				*error = "expected controller version encode failure";
				return false;
			}

			NetLockstepFrame frame;
			frame.senderPeerId = 1;
			frame.targetFrame = 10;
			frame.frames = {MakeFrame(200, 1), MakeFrame(100, 2)};
			if (NetLockstepCodec::Encode({frame}, bytes, &encodeError) || encodeError.code != NetLockstepErrorCode::InvalidValue) {
				*error = "expected unsorted frame encode failure";
				return false;
			}

			frame.frames = {MakeFrame(100, 1), MakeFrame(100, 2)};
			if (NetLockstepCodec::Encode({frame}, bytes, &encodeError) || encodeError.code != NetLockstepErrorCode::InvalidValue) {
				*error = "expected duplicate frame encode failure";
				return false;
			}

			frame.frames = {MakeFrame(100, 1)};
			frame.frames[0].flags = 0x80U;
			if (NetLockstepCodec::Encode({frame}, bytes, &encodeError) || encodeError.code != NetLockstepErrorCode::InvalidValue) {
				*error = "expected invalid ControllerFrame encode failure";
				return false;
			}

			if (!EncodePacket({NetLockstepStop{1, NetLockstepStopReason::InternalError, 0, "x"}}, bytes, error)) {
				return false;
			}
			std::vector<uint8_t> mutated = bytes;
			mutated[NetLockstepCodec::c_HeaderBytes + 2] = 0xFEU;
			mutated[NetLockstepCodec::c_HeaderBytes + 3] = 0xFFU;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::InvalidValue, error)) {
				return false;
			}
			return true;
		}

		bool StartCoordinatorPair(uint16_t port, LoopbackTransport& hostTransport, LoopbackTransport& clientTransport, NetLockstepCoordinator& host, NetLockstepCoordinator& client, NetLockstepConfig hostConfig, NetLockstepConfig clientConfig, std::string* error) {
			if (!hostTransport.StartHost(port, error)) {
				return false;
			}
			if (!clientTransport.Connect("loopback", port, error)) {
				return false;
			}
			hostConfig.remoteTransportPeerId = 1;
			clientConfig.remoteTransportPeerId = 1;
			if (!host.Start(hostTransport, hostConfig, error)) {
				return false;
			}
			if (!client.Start(clientTransport, clientConfig, error)) {
				return false;
			}
			return true;
		}

		NetLockstepConfig MakeCoordinatorConfig(uint8_t localPeerId, uint8_t remotePeerId, uint64_t sessionId, uint16_t inputDelayFrames, NetTransportLane frameLane) {
			NetLockstepConfig config;
			config.sessionId = sessionId;
			config.startFrame = 0;
			config.inputDelayFrames = inputDelayFrames;
			config.timeoutMs = 250;
			config.localPeerId = localPeerId;
			config.remotePeerId = remotePeerId;
			config.peerCount = 2;
			config.frameLane = frameLane;
			config.scenario = "LockstepSelfTest";
			config.ownershipPolicy = "unique-id-split";
			return config;
		}

		void DrainReady(NetLockstepCoordinator& coordinator, std::vector<uint64_t>& readyFrames) {
			NetLockstepReadyFrame ready;
			while (coordinator.PopReadyFrame(ready)) {
				readyFrames.push_back(ready.frame);
			}
		}

		bool DriveCoordinators(LoopbackTransport& hostTransport, LoopbackTransport& clientTransport, NetLockstepCoordinator& host, NetLockstepCoordinator& client, const std::function<bool()>& done, std::string* error, uint64_t maxMs = 1000, uint64_t stepMs = 5) {
			for (uint64_t now = 0; now <= maxMs; now += stepMs) {
				host.Tick(now);
				client.Tick(now);
				if (done()) {
					return true;
				}
				hostTransport.AdvanceTimeMs(stepMs);
				clientTransport.AdvanceTimeMs(stepMs);
			}
			*error = "condition not reached; host=" + std::string(NetLockstepCoordinator::StateName(host.GetState())) +
			         " client=" + NetLockstepCoordinator::StateName(client.GetState()) +
			         " host_report=" + host.BuildReportJson();
			return false;
		}

		bool TestRecoveryStopsAtCompletedTick(std::string* error) {
			for (uint16_t delay: {0, 3}) {
				for (bool rejoin: {false, true}) {
					LoopbackTransport hostTransport, clientTransport;
					NetLockstepCoordinator host, client;
					const uint16_t port = 43920 + delay + (rejoin ? 10 : 0);
					if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client,
					    MakeCoordinatorConfig(1, 2, port, delay, NetTransportLane::ControlReliable),
					    MakeCoordinatorConfig(2, 1, port, delay, NetTransportLane::ControlReliable), error)) return false;
					host.DeferStopsToTickBoundary();
					client.DeferStopsToTickBoundary();
					if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error)) return false;
					if (!host.QueueLocalInput(0, {MakeFrame(100, 1)}, {}, error) || !client.QueueLocalInput(0, {MakeFrame(101, 1)}, {}, error)) return false;
					if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.GetStats().framesAccepted == 1 && client.GetStats().framesAccepted == 1; }, error)) return false;
					if (rejoin) {
						host.RequestResync("player rejoined");
					} else {
						std::array<uint8_t, 32> first{}, second{};
						second[0] = 1;
						if (!host.SubmitLocalChecksum(delay, first, error) || !client.SubmitLocalChecksum(delay, second, error)) return false;
					}
					if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.HasPendingRecoveryStop(); }, error)) return false;
					if (!host.IsRunning() || !client.IsRunning()) {
						*error = "recovery stopped input before the authoritative tick completed";
						return false;
					}
					if (!host.QueueLocalInput(1, {MakeFrame(100, 2)}, {}, error) || !client.QueueLocalInput(1, {MakeFrame(101, 2)}, {}, error)) return false;
					if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.GetStats().framesAccepted == 2 && client.GetStats().framesAccepted == 2; }, error)) return false;
					if (client.FinishSimulationTick(delay + 1) || !host.FinishSimulationTick(delay + 1) || !host.IsFailed()) {
						*error = "the host did not own the recovery boundary";
						return false;
					}
					if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return client.IsFailed(); }, error)) return false;
					const std::string reason = rejoin ? "ResyncRequested:" : "Desync:";
					if (!host.GetStats().timeoutReason.starts_with(reason) || !client.GetStats().timeoutReason.starts_with(reason)) {
						*error = "recovery lost its reason";
						return false;
					}
				}
			}
			std::cout << "[net-lockstep-selftest] PASS recovery_stops_at_completed_tick D=0,3 desync/rejoin" << std::endl;
			return true;
		}

		bool TestCompletionDrainsAppliedTicks(std::string* error) {
			for (uint16_t delay: {0, 3}) {
				LoopbackTransport hostTransport, clientTransport;
				NetLockstepCoordinator host, client;
				const uint16_t port = 43940 + delay;
				if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client,
				    MakeCoordinatorConfig(1, 2, port, delay, NetTransportLane::ControlReliable),
				    MakeCoordinatorConfig(2, 1, port, delay, NetTransportLane::ControlReliable), error)) return false;
				host.DeferStopsToTickBoundary();
				client.DeferStopsToTickBoundary();
				if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error)) return false;
				for (uint64_t produced = 0; produced < 2; ++produced) {
					if (!host.QueueLocalInput(produced, {MakeFrame(100, produced)}, {}, error) || !client.QueueLocalInput(produced, {MakeFrame(101, produced)}, {}, error)) return false;
				}
				if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.GetStats().framesAccepted == 2 && client.GetStats().framesAccepted == 2; }, error)) return false;
				host.FinishSimulationTick(delay);
				host.Complete("finished at applied tick");
				for (int poll = 0; poll < 10; ++poll) {
					hostTransport.AdvanceTimeMs(1);
					clientTransport.AdvanceTimeMs(1);
					client.Tick(clientTransport.NowMs());
				}
				if (!client.IsRunning()) {
					*error = "completion discarded an unapplied final simulation tick";
					return false;
				}
				NetLockstepReadyFrame finalFrame;
				if (!client.PopReadyFrame(finalFrame) || finalFrame.frame != delay || !client.FinishSimulationTick(delay) || !client.IsStopped()) {
					*error = "completion used prefetched input instead of the sender's completed simulation tick";
					return false;
				}
				if (client.GetStats().timeoutReason != "Complete:finished at applied tick") return false;
			}
			std::cout << "[net-lockstep-selftest] PASS completion_drains_applied_ticks D=0,3" << std::endl;
			return true;
		}

		bool TestCoordinatorDelayedHappyPath(std::string* error) {
			const uint16_t port = 43001;
			const uint64_t sessionId = 0x7000000000000001ULL;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client,
			                          MakeCoordinatorConfig(1, 2, sessionId, 2, NetTransportLane::ControlReliable),
			                          MakeCoordinatorConfig(2, 1, sessionId, 2, NetTransportLane::ControlReliable), error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error)) {
				return false;
			}
			if (host.GetStats().effectiveStartFrame != 2 || client.GetStats().effectiveStartFrame != 2) {
				*error = "input-delay effective start frame was not applied";
				return false;
			}

			const NetGameCommand fundsCommand{1, NetGameSetTeamFunds{0, 4200}};
			for (uint64_t producedFrame = 0; producedFrame < 5; ++producedFrame) {
				const std::vector<NetGameCommand> hostCommands = producedFrame == 0 ? std::vector<NetGameCommand>{fundsCommand} : std::vector<NetGameCommand>{};
				if (!host.QueueLocalInput(producedFrame, {MakeFrame(100 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, hostCommands, error) ||
				    !client.QueueLocalInput(producedFrame, {MakeFrame(200 + static_cast<int64_t>(producedFrame), producedFrame + 11)}, {}, error)) {
					return false;
				}
			}

			std::vector<uint64_t> hostReady;
			std::vector<uint64_t> clientReady;
			std::vector<NetGameCommand> hostLocalCommands;
			std::vector<NetGameCommand> clientRemoteCommands;
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] {
					NetLockstepReadyFrame ready;
					while (host.PopReadyFrame(ready)) {
						hostReady.push_back(ready.frame);
						hostLocalCommands.insert(hostLocalCommands.end(), ready.localCommands.begin(), ready.localCommands.end());
					}
					while (client.PopReadyFrame(ready)) {
						clientReady.push_back(ready.frame);
						clientRemoteCommands.insert(clientRemoteCommands.end(), ready.remoteCommands.begin(), ready.remoteCommands.end());
					}
					return hostReady.size() == 5 && clientReady.size() == 5;
				}, error)) {
				return false;
			}
			if (hostReady.front() != 2 || clientReady.front() != 2 || host.GetStats().framesAccepted != 5 || client.GetStats().framesAccepted != 5) {
				*error = "delayed lockstep did not accept the expected ready frames";
				return false;
			}
			// The host's funds command must surface in its own ready frame (localCommands) and on the client (remoteCommands).
			if (hostLocalCommands.size() != 1 || !(hostLocalCommands.front() == fundsCommand) ||
			    clientRemoteCommands.size() != 1 || !(clientRemoteCommands.front() == fundsCommand)) {
				*error = "game command did not surface in the synced ready frame";
				return false;
			}
			const std::string report = host.BuildReportJson();
			if (report.find("\"input_delay_frames\":2") == std::string::npos ||
			    report.find("\"frames_accepted\":5") == std::string::npos ||
			    report.find("\"timeouts\":0") == std::string::npos) {
				*error = "happy-path report is missing deterministic stats";
				return false;
			}
			return true;
		}

		// A resync restarts lockstep while a peer is still reloading: the host's start reaches a
		// transport nobody is polling for it, and the host's first frames then arrive before the
		// peer ever sees a start. The frames must wait for the retransmitted start, not fail the round.
		bool TestCoordinatorFrameBeforeStart(std::string* error) {
			const uint16_t port = 43011;
			const uint64_t sessionId = 0x7000000000000011ULL;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			if (!hostTransport.StartHost(port, error) || !clientTransport.Connect("loopback", port, error)) {
				return false;
			}
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 2, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 2, NetTransportLane::ControlReliable);
			hostConfig.roundId = 0x9A5E000000000007ULL;
			hostConfig.remoteTransportPeerId = 1;
			clientConfig.remoteTransportPeerId = 1;
			// The missing-frame grace must outlast the start retransmit cycle, as a real match's does.
			hostConfig.timeoutMs = 2000;
			clientConfig.timeoutMs = 2000;
			// One clock for the whole scenario; a wait that spans two drives must not see time go backwards.
			uint64_t now = 0;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 10, now += 10) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					hostTransport.AdvanceTimeMs(10);
					clientTransport.AdvanceTimeMs(10);
				}
				*error = "condition not reached; host=" + host.BuildReportJson() + " client=" + client.BuildReportJson();
				return false;
			};
			if (!host.Start(hostTransport, hostConfig, error)) {
				return false;
			}
			// The client is between rounds: its transport drains the host's start into the void.
			clientTransport.AdvanceTimeMs(10);
			if (clientTransport.PollEvents().empty()) {
				*error = "the host start did not reach the client transport";
				return false;
			}
			if (!client.Start(clientTransport, clientConfig, error)) {
				return false;
			}
			if (!drive([&] { return host.IsRunning(); }, 100)) {
				return false;
			}
			for (uint64_t producedFrame = 0; producedFrame < 3; ++producedFrame) {
				if (!host.QueueLocalInput(producedFrame, {MakeFrame(100 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, {}, error)) {
					return false;
				}
			}
			if (!drive([&] { return client.GetStats().preStartFramesBuffered == 3; }, 100)) {
				return false;
			}
			if (client.IsFailed() || client.IsRunning()) {
				*error = "frames that outran the start were not held (" + client.BuildReportJson() + ")";
				return false;
			}
			if (!drive([&] { return client.IsRunning(); }, 1000)) {
				return false;
			}
			for (uint64_t producedFrame = 0; producedFrame < 3; ++producedFrame) {
				if (!client.QueueLocalInput(producedFrame, {MakeFrame(200 + static_cast<int64_t>(producedFrame), producedFrame + 11)}, {}, error)) {
					return false;
				}
			}
			std::vector<uint64_t> hostReady;
			std::vector<uint64_t> clientReady;
			if (!drive([&] {
					DrainReady(host, hostReady);
					DrainReady(client, clientReady);
					return hostReady.size() == 3 && clientReady.size() == 3;
				}, 1000)) {
				return false;
			}
			if (host.GetStats().startRetransmits == 0 || client.GetStats().startRetransmits == 0 || client.GetRoundId() != hostConfig.roundId) {
				*error = "the missed start was not repeated (" + host.BuildReportJson() + " / " + client.BuildReportJson() + ")";
				return false;
			}
			if (hostReady != std::vector<uint64_t>{2, 3, 4} || clientReady != std::vector<uint64_t>{2, 3, 4}) {
				*error = "held frames did not commit after the start";
				return false;
			}
			return true;
		}

		// A frame or checksum tagged with another round is a straggler from before a resync: ignored, never applied or failed on.
		bool TestCoordinatorIgnoresStaleRound(std::string* error) {
			const uint16_t port = 43012;
			const uint64_t sessionId = 0x7000000000000012ULL;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 2, NetTransportLane::ControlReliable);
			hostConfig.roundId = 0x9A5E000000000008ULL;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig,
			                          MakeCoordinatorConfig(2, 1, sessionId, 2, NetTransportLane::ControlReliable), error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error)) {
				return false;
			}
			if (client.GetRoundId() != hostConfig.roundId) {
				*error = "client did not adopt the host's round";
				return false;
			}
			NetLockstepFrame stale;
			stale.senderPeerId = 1;
			stale.targetFrame = 2;
			stale.roundId = 0x9A5E000000000001ULL;
			stale.frames = {MakeFrame(100, 1)};
			std::vector<uint8_t> bytes;
			if (!EncodePacket({stale}, bytes, error) || !hostTransport.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			NetLockstepChecksum staleChecksum;
			staleChecksum.senderPeerId = 1;
			staleChecksum.frame = 2;
			staleChecksum.roundId = 0x9A5E000000000001ULL;
			if (!EncodePacket({staleChecksum}, bytes, error) || !hostTransport.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return client.GetStats().staleRoundPackets == 2; }, error)) {
				return false;
			}
			if (client.IsFailed() || client.GetStats().remoteControllerFramesReceived != 0) {
				*error = "a stale-round packet was applied or failed the round";
				return false;
			}
			for (uint64_t producedFrame = 0; producedFrame < 3; ++producedFrame) {
				if (!host.QueueLocalInput(producedFrame, {MakeFrame(100 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, {}, error) ||
				    !client.QueueLocalInput(producedFrame, {MakeFrame(200 + static_cast<int64_t>(producedFrame), producedFrame + 11)}, {}, error)) {
					return false;
				}
			}
			std::vector<uint64_t> hostReady;
			std::vector<uint64_t> clientReady;
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] {
					DrainReady(host, hostReady);
					DrainReady(client, clientReady);
					return hostReady.size() == 3 && clientReady.size() == 3;
				}, error)) {
				return false;
			}
			return true;
		}

		// P8-1: each sender runs its OWN input delay. The commit stream starts at the earliest
		// sender's first delayed frame; slower senders ramp in, and every peer must build the
		// byte-identical per-frame apply set (local + remote union) through the ramp-in window.
		bool TestCoordinatorPerSenderDelay(std::string* error) {
			const uint16_t port = 43005;
			const uint64_t sessionId = 0x7000000000000005ULL;
			const std::map<uint8_t, uint16_t> delays = {{1, 1}, {2, 3}};

			// A local delay that disagrees with the per-peer set must be rejected up front.
			{
				LoopbackTransport rejectTransport;
				if (!rejectTransport.StartHost(43006, error)) {
					return false;
				}
				NetLockstepConfig bad = MakeCoordinatorConfig(1, 2, sessionId, 2, NetTransportLane::ControlReliable);
				bad.peerInputDelayFrames = delays;
				bad.remoteTransportPeerId = 1;
				NetLockstepCoordinator reject;
				std::string rejectError;
				if (reject.Start(rejectTransport, bad, &rejectError) || rejectError.find("disagrees") == std::string::npos) {
					*error = "mismatched local delay was not rejected at start";
					return false;
				}
			}

			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 1, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 3, NetTransportLane::ControlReliable);
			hostConfig.peerInputDelayFrames = delays;
			clientConfig.peerInputDelayFrames = delays;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error)) {
				return false;
			}
			if (host.GetStats().effectiveStartFrame != 1 || client.GetStats().effectiveStartFrame != 1) {
				*error = "per-sender commit stream did not start at the earliest sender's delay";
				return false;
			}

			for (uint64_t producedFrame = 0; producedFrame < 5; ++producedFrame) {
				if (!host.QueueLocalInput(producedFrame, {MakeFrame(100 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, {}, error) ||
				    !client.QueueLocalInput(producedFrame, {MakeFrame(200 + static_cast<int64_t>(producedFrame), producedFrame + 11)}, {}, error)) {
					return false;
				}
			}

			std::map<uint64_t, std::vector<int64_t>> hostUnions;
			std::map<uint64_t, std::vector<int64_t>> clientUnions;
			auto collectUnions = [](NetLockstepCoordinator& coordinator, std::map<uint64_t, std::vector<int64_t>>& out) {
				NetLockstepReadyFrame ready;
				while (coordinator.PopReadyFrame(ready)) {
					std::vector<int64_t> ids;
					for (const ControllerFrame& controllerFrame: ready.localFrames) {
						ids.push_back(controllerFrame.actorUniqueID);
					}
					for (const ControllerFrame& controllerFrame: ready.remoteFrames) {
						ids.push_back(controllerFrame.actorUniqueID);
					}
					std::sort(ids.begin(), ids.end());
					out[ready.frame] = std::move(ids);
				}
			};
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] {
					collectUnions(host, hostUnions);
					collectUnions(client, clientUnions);
					return hostUnions.size() >= 5 && clientUnions.size() >= 5;
				}, error)) {
				return false;
			}
			// Frames 1-2 carry the host alone (the client is still inside its delay); 3-5 carry both.
			if (hostUnions.begin()->first != 1 || clientUnions.begin()->first != 1 || hostUnions != clientUnions) {
				*error = "per-sender ramp-in did not produce identical apply sets on both peers";
				return false;
			}
			if (hostUnions[1] != std::vector<int64_t>{100} || hostUnions[3] != std::vector<int64_t>{102, 200}) {
				*error = "per-sender ramp-in merged the wrong sender sets";
				return false;
			}
			if (host.BuildReportJson().find("\"peer_input_delays\":{\"1\":1,\"2\":3}") == std::string::npos) {
				*error = "per-peer input delays are missing from the report";
				return false;
			}
			return true;
		}

		// A peer whose announced delay disagrees with the shared per-peer set must fail the start.
		bool TestCoordinatorPerSenderDelayMismatch(std::string* error) {
			const uint16_t port = 43007;
			const uint64_t sessionId = 0x7000000000000007ULL;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 1, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 2, NetTransportLane::ControlReliable);
			hostConfig.peerInputDelayFrames = {{1, 1}, {2, 3}};
			clientConfig.peerInputDelayFrames = {{1, 1}, {2, 2}};
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsFailed(); }, error)) {
				return false;
			}
			if (host.GetStats().timeoutReason.find("start mismatch") == std::string::npos) {
				*error = "per-sender delay mismatch did not fail the start";
				return false;
			}
			return true;
		}

		bool TestCoordinatorIgnoresSessionPacketsAtHandoff(std::string* error) {
			const uint16_t port = 43004;
			const uint64_t sessionId = 0x7000000000000004ULL;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			if (!hostTransport.StartHost(port, error) || !clientTransport.Connect("loopback", port, error)) {
				return false;
			}

			std::vector<uint8_t> heartbeatBytes;
			NetProtocolError protocolError;
			if (!NetProtocol::Encode({99, 0, NetHeartbeat{10, 2, 3}}, heartbeatBytes, &protocolError)) {
				*error = "session heartbeat encode failed: " + protocolError.message;
				return false;
			}
			if (!hostTransport.Send(1, NetTransportLane::ControlReliable, heartbeatBytes, error) ||
			    !clientTransport.Send(1, NetTransportLane::ControlReliable, heartbeatBytes, error)) {
				return false;
			}

			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::ControlReliable);
			hostConfig.remoteTransportPeerId = 1;
			clientConfig.remoteTransportPeerId = 1;
			if (!host.Start(hostTransport, hostConfig, error) || !client.Start(clientTransport, clientConfig, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error)) {
				return false;
			}
			if (host.GetStats().ignoredSessionPackets == 0 || client.GetStats().ignoredSessionPackets == 0) {
				*error = "coordinator did not report ignored session packets at handoff";
				return false;
			}
			return true;
		}

		bool TestCoordinatorUnreliableOutOfOrderDuplicate(std::string* error) {
			const uint16_t port = 43002;
			const uint64_t sessionId = 0x7000000000000002ULL;
			LoopbackTransportConfig faults;
			faults.latencyMs = 2;
			faults.jitterMs = 5;
			faults.reorderUnreliable = true;
			faults.unreliableDuplicateEveryN = 2;

			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			hostTransport.SetFaultConfig(faults);
			clientTransport.SetFaultConfig(faults);
			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client,
			                          MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::InputUnreliable),
			                          MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::InputUnreliable), error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error)) {
				return false;
			}

			for (uint64_t producedFrame : {1ULL, 0ULL, 2ULL, 3ULL}) {
				if (!host.QueueLocalInput(producedFrame, {MakeFrame(100 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, {}, error) ||
				    !client.QueueLocalInput(producedFrame, {MakeFrame(200 + static_cast<int64_t>(producedFrame), producedFrame + 11)}, {}, error)) {
					return false;
				}
			}

			std::vector<uint64_t> hostReady;
			std::vector<uint64_t> clientReady;
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] {
					DrainReady(host, hostReady);
					DrainReady(client, clientReady);
					return hostReady.size() == 4 && clientReady.size() == 4 &&
					       host.GetStats().duplicateFrames > 0 && client.GetStats().duplicateFrames > 0;
				}, error, 1500)) {
				return false;
			}
			if (hostReady != std::vector<uint64_t>{0, 1, 2, 3} || clientReady != std::vector<uint64_t>{0, 1, 2, 3}) {
				*error = "out-of-order frames were not released in frame order";
				return false;
			}
			if (host.GetStats().outOfOrderFrames == 0 || client.GetStats().outOfOrderFrames == 0) {
				*error = "out-of-order frame handling was not exercised";
				return false;
			}
			return true;
		}

		bool TestCoordinatorMissingFrameTimeout(std::string* error) {
			const uint16_t port = 43003;
			const uint64_t sessionId = 0x7000000000000003ULL;
			LoopbackTransportConfig clientFaults;
			clientFaults.unreliableDropEveryN = 2;

			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			clientTransport.SetFaultConfig(clientFaults);
			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::InputUnreliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::InputUnreliable);
			hostConfig.timeoutMs = 40;
			clientConfig.timeoutMs = 40;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error)) {
				return false;
			}
			if (!host.QueueLocalInput(0, {MakeFrame(100, 1)}, {}, error) ||
			    !client.QueueLocalInput(0, {MakeFrame(200, 2)}, {}, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsFailed(); }, error, 500)) {
				return false;
			}
			if (host.GetStats().missingFrameStalls == 0 ||
			    host.GetStats().timeoutReason.find("MissingFrameTimeout") == std::string::npos ||
			    host.BuildReportJson().find("\"timeouts\":1") == std::string::npos) {
				*error = "missing-frame timeout did not produce the expected report";
				return false;
			}
			return true;
		}

		// P4C: three peers over a host-star loopback (clients connect only to the host, which relays).
		// Proves N-peer frame collection (advance only when all remotes are in), the peerId-ordered
		// merge, all-starts-before-run, and N-way checksum agreement.
		bool TestCoordinatorThreePeer(std::string* error) {
			const uint16_t port = 43010;
			const uint64_t sessionId = 0x7000000000000010ULL;
			LoopbackTransport hostT, clientAT, clientBT;
			if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) || !clientBT.Connect("loopback", port, error)) {
				return false;
			}
			// Client A connected first (host-side transport id 1), client B second (id 2). Clients see
			// the host as transport id 1. Peers: host=1, clientA=2, clientB=3.
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.startFrame = 0;
				c.inputDelayFrames = 0;
				c.timeoutMs = 500;
				c.localPeerId = local;
				c.peerCount = 3;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.frameLane = NetTransportLane::ControlReliable;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			NetLockstepCoordinator host, clientA, clientB;
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error) ||
			    !clientA.Start(clientAT, cfg(2, {{1, 1}}, false), error) ||
			    !clientB.Start(clientBT, cfg(3, {{1, 1}}, false), error)) {
				return false;
			}
			auto drive = [&](const std::function<bool()>& done) {
				for (uint64_t now = 0; now <= 2000; now += 5) {
					host.Tick(now);
					clientA.Tick(now);
					clientB.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					clientAT.AdvanceTimeMs(5);
					clientBT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && clientA.IsRunning() && clientB.IsRunning(); })) {
				*error = "three-peer lockstep did not reach Running (start relay failed)";
				return false;
			}
			for (uint64_t f = 0; f < 4; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100 + static_cast<int64_t>(f), f + 1)}, {}, error) ||
				    !clientA.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error) ||
				    !clientB.QueueLocalInput(f, {MakeFrame(300 + static_cast<int64_t>(f), f + 1)}, {}, error)) {
					return false;
				}
			}
			std::vector<uint64_t> hostReady, aReady, bReady;
			auto collect = [&](NetLockstepCoordinator& c, std::vector<uint64_t>& out, size_t& mergedRemotes) {
				NetLockstepReadyFrame ready;
				while (c.PopReadyFrame(ready)) {
					out.push_back(ready.frame);
					mergedRemotes = ready.remoteFrames.size();
				}
			};
			size_t hostRemotes = 0, aRemotes = 0, bRemotes = 0;
			if (!drive([&] {
					collect(host, hostReady, hostRemotes);
					collect(clientA, aReady, aRemotes);
					collect(clientB, bReady, bRemotes);
					return hostReady.size() >= 4 && aReady.size() >= 4 && bReady.size() >= 4;
				})) {
				*error = "three-peer lockstep did not produce 4 ready frames on every peer";
				return false;
			}
			// Every peer merges the two OTHER peers' frames into remoteFrames each tick.
			if (hostRemotes != 2 || aRemotes != 2 || bRemotes != 2) {
				*error = "three-peer ready frame did not merge both remote peers";
				return false;
			}
			// N-way checksum: all three agree on frame 0 -> verified, no desync.
			std::array<uint8_t, 32> hash{};
			hash.fill(0x5A);
			if (!host.SubmitLocalChecksum(0, hash, error) || !clientA.SubmitLocalChecksum(0, hash, error) || !clientB.SubmitLocalChecksum(0, hash, error)) {
				return false;
			}
			drive([&] { return false; });
			if (host.IsFailed() || clientA.IsFailed() || clientB.IsFailed()) {
				*error = "three-peer matching checksums wrongly desynced";
				return false;
			}
			return true;
		}

		bool TestCoordinatorPeerLeave(std::string* error) {
			const uint16_t port = 43011;
			const uint64_t sessionId = 0x7000000000000011ULL;
			LoopbackTransport hostT, clientAT, clientBT;
			if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) || !clientBT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.timeoutMs = 500;
				c.localPeerId = local;
				c.peerCount = 3;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			NetLockstepCoordinator host, clientA, clientB;
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error) ||
			    !clientA.Start(clientAT, cfg(2, {{1, 1}}, false), error) ||
			    !clientB.Start(clientBT, cfg(3, {{1, 1}}, false), error)) {
				return false;
			}
			auto drive = [&](const std::function<bool()>& done) {
				for (uint64_t now = 0; now <= 2000; now += 5) {
					host.Tick(now);
					clientA.Tick(now);
					clientB.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					clientAT.AdvanceTimeMs(5);
					clientBT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && clientA.IsRunning() && clientB.IsRunning(); })) {
				*error = "leave test did not reach Running";
				return false;
			}
			// Everyone produces frames 0-1, then B leaves cleanly.
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !clientA.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error) ||
				    !clientB.QueueLocalInput(f, {MakeFrame(300, f + 1)}, {}, error)) {
					return false;
				}
			}
			clientB.Leave("bye");
			if (!clientB.IsStopped()) {
				*error = "leaver did not stop after Leave";
				return false;
			}
			// The survivors keep producing; frames 2-3 must advance WITHOUT B.
			for (uint64_t f = 2; f < 4; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !clientA.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error)) {
					return false;
				}
			}
			std::map<uint64_t, size_t> hostRemotesByFrame, aRemotesByFrame;
			auto collect = [](NetLockstepCoordinator& c, std::map<uint64_t, size_t>& out) {
				NetLockstepReadyFrame ready;
				while (c.PopReadyFrame(ready)) {
					out[ready.frame] = ready.remoteFrames.size();
				}
			};
			if (!drive([&] {
					collect(host, hostRemotesByFrame);
					collect(clientA, aRemotesByFrame);
					return hostRemotesByFrame.size() >= 4 && aRemotesByFrame.size() >= 4;
				})) {
				*error = "survivors did not advance past the leaver (host=" + std::to_string(hostRemotesByFrame.size()) +
				         " a=" + std::to_string(aRemotesByFrame.size()) + ")";
				return false;
			}
			if (host.IsFailed() || clientA.IsFailed()) {
				*error = "a survivor failed after a clean leave";
				return false;
			}
			// Frames through the leaver's last one carry its data; later frames drop to one remote.
			if (hostRemotesByFrame[1] != 2 || hostRemotesByFrame[2] != 1 || aRemotesByFrame[1] != 2 || aRemotesByFrame[2] != 1) {
				*error = "leave boundary merged the wrong remote sets";
				return false;
			}
			if (host.GetPeerLeaveFrames().count(3) == 0 || clientA.GetPeerLeaveFrames().count(3) == 0) {
				*error = "survivors did not record the leaver";
				return false;
			}
			// A 2-peer leave ends the peer's match: with B gone, A leaving leaves the host alone.
			clientA.Leave("bye too");
			drive([&] { return host.IsStopped(); });
			if (!host.IsStopped()) {
				*error = "host did not stop after every peer left";
				return false;
			}
			return true;
		}

		// A host-star fixture on the injected clock: host=1, clientA=2, clientB=3 over loopback.
		struct StarFixture {
			LoopbackTransport hostT, clientAT, clientBT;
			NetLockstepCoordinator host, clientA, clientB;
			uint64_t now = 0;
			uint64_t produced[3] = {0, 0, 0};

			bool Start(uint16_t port, uint64_t sessionId, uint32_t timeoutMs, std::string* error) {
				if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) || !clientBT.Connect("loopback", port, error)) {
					return false;
				}
				auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
					NetLockstepConfig c;
					c.sessionId = sessionId;
					c.timeoutMs = timeoutMs;
					c.localPeerId = local;
					c.peerCount = 3;
					c.remoteTransportPeerIds = std::move(transports);
					c.relayToOtherPeers = relay;
					c.scenario = "LockstepSelfTest";
					c.ownershipPolicy = "unique-id-split";
					return c;
				};
				return host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error) &&
				       clientA.Start(clientAT, cfg(2, {{1, 1}}, false), error) &&
				       clientB.Start(clientBT, cfg(3, {{1, 1}}, false), error);
			}

			// Drives only the named peers, so a peer left out is one whose process has wedged: its
			// socket stays open and its packets stop.
			bool Drive(bool driveHost, bool driveA, bool driveB, const std::function<bool()>& done, uint64_t untilMs) {
				for (; now <= untilMs; now += 5) {
					if (driveHost) host.Tick(now);
					if (driveA) clientA.Tick(now);
					if (driveB) clientB.Tick(now);
					if (done()) {
						return true;
					}
					if (driveHost) hostT.AdvanceTimeMs(5);
					if (driveA) clientAT.AdvanceTimeMs(5);
					if (driveB) clientBT.AdvanceTimeMs(5);
				}
				return false;
			}

			// Produces while this peer's pipeline has room, and stops when it does not: a peer blocked
			// on a frame it cannot commit stops sending, exactly as the sim loop does while it waits.
			void Feed(NetLockstepCoordinator& peer, uint8_t peerId, int64_t actorId) {
				uint64_t& next = produced[peerId - 1];
				if (!peer.IsRunning() || next > peer.GetStats().nextFrame + 4) {
					return;
				}
				std::string ignored;
				if (peer.QueueLocalInput(next, {MakeFrame(actorId, next + 1)}, {}, &ignored)) {
					++next;
				}
			}

			// Every live peer keeps producing, as a real match does. Without this the injected clock
			// runs on while nobody sends, and a peer that has simply run out of scripted input reads
			// as wedged - which is the very thing these tests have to tell apart.
			bool DriveProducing(bool driveB, const std::function<bool()>& done, uint64_t untilMs) {
				return Drive(true, true, driveB, [&] {
					Feed(host, 1, 100);
					Feed(clientA, 2, 200);
					if (driveB) {
						Feed(clientB, 3, 300);
					}
					return done();
				}, untilMs);
			}

			void Collect(NetLockstepCoordinator& peer, std::vector<uint64_t>& out) {
				NetLockstepReadyFrame ready;
				while (peer.PopReadyFrame(ready)) {
					out.push_back(ready.frame);
				}
			}

			bool Running() { return host.IsRunning() && clientA.IsRunning() && clientB.IsRunning(); }
		};

		// A wedged client stops sending but keeps its socket, so the transport says nothing for as long
		// as its process lives - longer than every survivor's missing-frame grace. The relay host must
		// call it gone on its OWN bounded budget, or the healthy clients time out waiting for a peer
		// nobody has told them about. Only the host adjudicates: one relayed notice, one leave frame.
		bool TestCoordinatorHostAdjudicatesSilentPeer(std::string* error) {
			StarFixture fx;
			const uint32_t timeoutMs = 400;
			if (!fx.Start(43012, 0x7000000000000012ULL, timeoutMs, error)) {
				return false;
			}
			if (!fx.Drive(true, true, true, [&] { return fx.Running(); }, 2000)) {
				*error = "silent-peer fixture did not reach Running";
				return false;
			}
			std::vector<uint64_t> hostReady, aReady;
			if (!fx.DriveProducing(true, [&] {
					fx.Collect(fx.host, hostReady);
					fx.Collect(fx.clientA, aReady);
					return hostReady.size() >= 2 && aReady.size() >= 2;
				}, fx.now + timeoutMs)) {
				*error = "silent-peer fixture did not get the round moving";
				return false;
			}
			// B wedges here: never ticked again, never sends again. Host and A keep playing.
			const uint64_t silentFrom = fx.now;
			if (!fx.DriveProducing(false, [&] { return fx.clientA.GetPeerLeaveFrames().count(3) != 0; }, silentFrom + 4 * timeoutMs)) {
				*error = "the survivor never learned the wedged peer had left (host=" + fx.host.BuildReportJson() + ")";
				return false;
			}
			if (fx.now - silentFrom >= timeoutMs) {
				*error = "the drop notice reached the survivor after its own grace (" + std::to_string(fx.now - silentFrom) +
				         "ms of " + std::to_string(timeoutMs) + "ms)";
				return false;
			}
			if (fx.host.GetStats().peersDroppedSilent != 1 ||
			    fx.host.GetStats().timeoutReason.find("MissingFrameTimeout") != std::string::npos) {
				*error = "the host did not adjudicate the wedged peer: " + fx.host.BuildReportJson();
				return false;
			}
			// Every survivor drops the requirement at the SAME frame, or their committed sets diverge.
			if (fx.host.GetPeerLeaveFrames().at(3) != fx.clientA.GetPeerLeaveFrames().at(3)) {
				*error = "host and survivor disagreed on the leave frame";
				return false;
			}
			const size_t committedAtLeave = aReady.size();
			if (!fx.DriveProducing(false, [&] {
					fx.Collect(fx.host, hostReady);
					fx.Collect(fx.clientA, aReady);
					return aReady.size() >= committedAtLeave + 4 && hostReady.size() >= committedAtLeave + 4;
				}, fx.now + 4 * timeoutMs)) {
				*error = "the survivors did not keep playing past the wedged peer (host=" + std::to_string(hostReady.size()) +
				         " a=" + std::to_string(aReady.size()) + ")";
				return false;
			}
			if (fx.host.IsFailed() || fx.clientA.IsFailed() || fx.host.GetPeerLeaveFrames().count(2) != 0) {
				*error = "a survivor was failed or dropped after the wedged peer was adjudicated: " + fx.host.BuildReportJson();
				return false;
			}
			return true;
		}

		// The negative control for the rule above: a client NEVER adjudicates. Two survivors judging
		// independently would drop the same peer at different frames and diverge, so a client whose
		// host has gone quiet must still die on its own missing-frame grace, with the same text.
		bool TestCoordinatorSilentHostStillTimesOut(std::string* error) {
			StarFixture fx;
			const uint32_t timeoutMs = 300;
			if (!fx.Start(43013, 0x7000000000000013ULL, timeoutMs, error)) {
				return false;
			}
			if (!fx.Drive(true, true, true, [&] { return fx.Running(); }, 2000)) {
				*error = "silent-host fixture did not reach Running";
				return false;
			}
			std::vector<uint64_t> aReady;
			if (!fx.DriveProducing(true, [&] {
					fx.Collect(fx.clientA, aReady);
					return aReady.size() >= 2;
				}, fx.now + timeoutMs)) {
				*error = "silent-host fixture did not get the round moving";
				return false;
			}
			// The host wedges: A now waits on the host and, through it, on B.
			if (!fx.Drive(false, true, true, [&] { return fx.clientA.IsFailed(); }, fx.now + 6 * timeoutMs)) {
				*error = "a client with a silent host did not time out";
				return false;
			}
			if (fx.clientA.GetStats().timeoutReason.find("MissingFrameTimeout") == std::string::npos) {
				*error = "a client with a silent host stopped for the wrong reason: " + fx.clientA.GetStats().timeoutReason;
				return false;
			}
			if (!fx.clientA.GetPeerLeaveFrames().empty() || fx.clientA.GetStats().peersDroppedSilent != 0) {
				*error = "a client adjudicated a peer drop, which only the relay host may do";
				return false;
			}
			return true;
		}

		// Runs a 3-peer star to a moving round, then refuses every host send to clientB while all three
		// keep producing - so the ONLY thing wrong is the forward, not a peer that went quiet.
		bool StartRelayRefusal(StarFixture& fx, uint16_t port, uint64_t sessionId, uint32_t timeoutMs, std::vector<uint64_t>& hostReady, std::string* error) {
			if (!fx.Start(port, sessionId, timeoutMs, error)) {
				return false;
			}
			if (!fx.Drive(true, true, true, [&] { return fx.Running(); }, 2000)) {
				*error = "relay-refusal fixture did not reach Running";
				return false;
			}
			if (!fx.DriveProducing(true, [&] {
					fx.Collect(fx.host, hostReady);
					return hostReady.size() >= 2;
				}, fx.now + timeoutMs)) {
				*error = "relay-refusal fixture did not get the round moving";
				return false;
			}
			LoopbackTransportConfig refuseB;
			refuseB.refuseSendsToPeer = 2; // clientB's host-side transport id.
			fx.hostT.SetFaultConfig(refuseB);
			return true;
		}

		// A refused forward was never queued and the receiver cannot ask for it again, so the host
		// holds it and retries: a send buffer that frees up costs a hitch, not a player.
		bool TestCoordinatorRelayBacklogHeals(std::string* error) {
			StarFixture fx;
			const uint32_t timeoutMs = 2000; // Refusals are bounded at half of this; heal well inside it.
			std::vector<uint64_t> hostReady, bReady;
			if (!StartRelayRefusal(fx, 43014, 0x7000000000000014ULL, timeoutMs, hostReady, error)) {
				return false;
			}
			const uint64_t refusedFrom = fx.now;
			fx.DriveProducing(true, [&] { return fx.now >= refusedFrom + 200; }, refusedFrom + 200);
			if (fx.host.GetStats().relaySendFailures == 0) {
				*error = "the refusal never reached the relay: " + fx.host.BuildReportJson();
				return false;
			}
			fx.hostT.SetFaultConfig({});
			fx.Collect(fx.clientB, bReady);
			const size_t behind = bReady.size();
			if (!fx.DriveProducing(true, [&] {
					fx.Collect(fx.clientB, bReady);
					return bReady.size() >= behind + 4;
				}, fx.now + timeoutMs)) {
				*error = "a peer whose forwards were refused only briefly did not catch up: " + fx.host.BuildReportJson();
				return false;
			}
			const NetLockstepStats& stats = fx.host.GetStats();
			if (stats.relayResends == 0 || stats.peers.at(3).relayResends == 0) {
				*error = "the retried forwards were not counted: " + fx.host.BuildReportJson();
				return false;
			}
			if (!fx.host.GetPeerLeaveFrames().empty() || fx.host.IsFailed() || fx.clientB.IsFailed()) {
				*error = "a peer was dropped for a refusal that healed: " + fx.host.BuildReportJson();
				return false;
			}
			return true;
		}

		// A forward the transport keeps refusing IS a gap the receiver can never fill; it would hang
		// on that frame until its grace ran out and take the other clients with it. Bound it.
		bool TestCoordinatorRelayFailureDropsPeer(std::string* error) {
			StarFixture fx;
			const uint32_t timeoutMs = 400; // Refusals are bounded on the same budget as silence: 200ms.
			std::vector<uint64_t> hostReady, aReady;
			if (!StartRelayRefusal(fx, 43015, 0x7000000000000015ULL, timeoutMs, hostReady, error)) {
				return false;
			}
			if (!fx.DriveProducing(true, [&] { return fx.host.GetPeerLeaveFrames().count(3) != 0; }, fx.now + 4 * timeoutMs)) {
				*error = "the host kept a peer it could not reach: " + fx.host.BuildReportJson();
				return false;
			}
			const NetLockstepStats& stats = fx.host.GetStats();
			if (stats.relaySendFailures == 0 || stats.peers.at(3).relaySendFailures == 0 || stats.lastRelayError.empty()) {
				*error = "the refused relay was not counted: " + fx.host.BuildReportJson();
				return false;
			}
			// It has to be the unreachable bound: clientB's own frames keep reaching the host, so it
			// is never the silent one, and the two paths must stay distinguishable in the report.
			if (stats.peersDroppedSilent != 0 || stats.relayPacketsSent == 0 || stats.peers.at(2).relayPacketsSent == 0) {
				*error = "the peer was dropped for the wrong reason: " + fx.host.BuildReportJson();
				return false;
			}
			const size_t committedAtLeave = aReady.size();
			if (!fx.DriveProducing(true, [&] {
					fx.Collect(fx.host, hostReady);
					fx.Collect(fx.clientA, aReady);
					return aReady.size() >= committedAtLeave + 4;
				}, fx.now + 4 * timeoutMs)) {
				*error = "the survivors did not keep playing past the unreachable peer: " + fx.host.BuildReportJson();
				return false;
			}
			if (fx.host.IsFailed() || fx.clientA.IsFailed() || fx.clientA.GetPeerLeaveFrames().count(3) == 0) {
				*error = "the survivor did not follow the host past the unreachable peer";
				return false;
			}
			const std::string report = fx.host.BuildReportJson();
			if (report.find("\"relay_send_failures\":") == std::string::npos ||
			    report.find("\"peers\":{") == std::string::npos ||
			    report.find("\"frames_contributed\":") == std::string::npos) {
				*error = "the per-peer relay counters are missing from the report: " + report;
				return false;
			}
			return true;
		}

		// The H4 seat state the round asks for, stubbed. This case pins the coordinator half of the
		// hold; the plane's own answer is pinned by -net-reconnect-session-selftest.
		struct SeatStateStub {
			bool held = false;
			uint8_t heldPeerId = 0; //!< 0 holds every peer's seat; otherwise only this one's.
			NetPeerId fenced = c_InvalidNetPeerId;
		};

		NetLockstepSeatState QuerySeatStateStub(void* context, uint8_t peerId, NetPeerId transportPeerId) {
			auto* stub = static_cast<SeatStateStub*>(context);
			NetLockstepSeatState state;
			state.heldForReclaim = stub->held && (stub->heldPeerId == 0 || stub->heldPeerId == peerId);
			state.fencedTransport = transportPeerId != c_InvalidNetPeerId && transportPeerId == stub->fenced;
			return state;
		}

		// H4 §4: a 1v1 whose only remote DROPS holds its seat for the reclaim window instead of ending,
		// so the returner has a match to come back to. A clean leave with nobody left still ends at once.
		bool TestCoordinatorDroppedSeatHold(std::string* error) {
			uint16_t port = 43020;
			bool holdingBeforeDrop = false;
			bool holdingDuringHold = false;
			bool holdingAfterWindow = false;
			bool stillUsesDeadTransport = false;
			auto runDrop = [&](bool holdSeat, bool fenceTransport, bool cleanLeave, NetLockstepState& outState,
			                   size_t& outLeaves, std::string& outReason, uint64_t& outFramesAlone) {
				++port;
				const uint64_t sessionId = 0x7000000000000020ULL + port;
				LoopbackTransport hostT, clientT;
				std::string ignored;
				if (!hostT.StartHost(port, &ignored) || !clientT.Connect("loopback", port, &ignored)) {
					return false;
				}
				auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
					NetLockstepConfig c;
					c.sessionId = sessionId;
					c.timeoutMs = 5000;
					c.localPeerId = local;
					c.peerCount = 2;
					c.remoteTransportPeerIds = std::move(transports);
					c.relayToOtherPeers = relay;
					c.scenario = "LockstepSelfTest";
					c.ownershipPolicy = "unique-id-split";
					return c;
				};
				SeatStateStub stub;
				stub.held = holdSeat;
				NetLockstepCoordinator host, client;
				if (!host.Start(hostT, cfg(1, {{2, 1}}, true), &ignored) || !client.Start(clientT, cfg(2, {{1, 1}}, false), &ignored)) {
					return false;
				}
				host.SetSeatStateSource(&QuerySeatStateStub, &stub);
				uint64_t now = 0;
				auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
					for (const uint64_t until = now + forMs; now <= until; now += 5) {
						host.Tick(now);
						client.Tick(now);
						if (done()) {
							return true;
						}
						hostT.AdvanceTimeMs(5);
						clientT.AdvanceTimeMs(5);
					}
					return false;
				};
				if (!drive(2000, [&] { return host.IsRunning() && client.IsRunning(); })) {
					return false;
				}
				for (uint64_t f = 0; f < 2; ++f) {
					if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, &ignored) ||
					    !client.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, &ignored)) {
						return false;
					}
				}
				NetLockstepReadyFrame ready;
				size_t committed = 0;
				if (!drive(2000, [&] {
						while (host.PopReadyFrame(ready)) {
							++committed;
						}
						return committed >= 2;
					})) {
					return false;
				}
				// The drop: the transport goes away with no notice. A clean leave announces itself first.
				holdingBeforeDrop = host.IsHoldingSeatForReclaim();
				stub.fenced = fenceTransport ? static_cast<NetPeerId>(1) : c_InvalidNetPeerId;
				if (cleanLeave) {
					client.Leave("bye");
					drive(200, [] { return false; });
				}
				clientT.Stop();
				drive(200, [] { return false; });
				// Whatever the host decided, it must be able to keep producing frames on its own.
				outFramesAlone = 0;
				for (uint64_t f = 2; f < 5; ++f) {
					if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, &ignored)) {
						break;
					}
				}
				drive(200, [&] {
					while (host.PopReadyFrame(ready)) {
						++outFramesAlone;
					}
					return false;
				});
				outState = host.GetState();
				outLeaves = host.GetPeerLeaveFrames().size();
				outReason = host.GetStats().timeoutReason;
				// A5: the activity gate reads exactly this - the round is alive only for a held seat.
				holdingDuringHold = host.IsHoldingSeatForReclaim();
				// A6: whatever the round decided, it must stop naming a transport that is gone.
				stillUsesDeadTransport = host.UsesTransportPeer(static_cast<NetPeerId>(1));
				if (outState == NetLockstepState::Running && !cleanLeave && !fenceTransport) {
					// The window closes: the very next Tick must end a round nobody is coming back to.
					stub.held = false;
					host.Tick(now + 5);
					outState = host.GetState();
					outReason = host.GetStats().timeoutReason;
					holdingAfterWindow = host.IsHoldingSeatForReclaim();
				}
				return true;
			};

			NetLockstepState state = NetLockstepState::Idle;
			size_t leaves = 0;
			std::string reason;
			uint64_t framesAlone = 0;

			// Held: the round plays on without the dropped peer, then ends when the window closes.
			if (!runDrop(true, false, false, state, leaves, reason, framesAlone)) {
				*error = "the held-seat drop fixture did not run";
				return false;
			}
			if (leaves != 1) {
				*error = "a held drop did not stop requiring the dropped peer's frames";
				return false;
			}
			if (framesAlone == 0) {
				*error = "the host produced nothing while it held the seat";
				return false;
			}
			if (state != NetLockstepState::Stopped || reason.rfind("PeerLeft:", 0) != 0) {
				*error = "the round did not end once the reclaim window closed: " + reason;
				return false;
			}
			// A5: the activity may only be held open while the round itself is being held open.
			if (holdingBeforeDrop || !holdingDuringHold || holdingAfterWindow) {
				*error = "the held-seat window was not visible to the activity gate";
				return false;
			}

			// The control: with no seat held this is exactly the old behaviour - the drop ends the match.
			uint64_t controlFrames = 0;
			if (!runDrop(false, false, false, state, leaves, reason, controlFrames)) {
				*error = "the unheld-seat control did not run";
				return false;
			}
			if (state != NetLockstepState::Stopped || reason.rfind("PeerLeft:", 0) != 0) {
				*error = "an unheld 1v1 drop no longer ends the match: " + reason;
				return false;
			}
			if (holdingDuringHold) {
				*error = "a round nobody is coming back to reported itself as holding a seat";
				return false;
			}
			if (controlFrames != 0) {
				*error = "the host kept producing frames after an unheld drop";
				return false;
			}

			// A clean leave with nobody left ends the match at once even while the seat would be held.
			if (!runDrop(true, false, true, state, leaves, reason, framesAlone)) {
				*error = "the clean-leave fixture did not run";
				return false;
			}
			if (state != NetLockstepState::Stopped || reason.rfind("PeerLeft:", 0) != 0) {
				*error = "a clean 1v1 leave no longer ends the match: " + reason;
				return false;
			}
			if (holdingDuringHold) {
				*error = "an announced leave still reported the round as holding its seat";
				return false;
			}

			// A superseded incarnation's socket closing is not a leave at all: the seat's live holder is
			// another transport, so the round keeps requiring it.
			if (!runDrop(false, true, false, state, leaves, reason, framesAlone)) {
				*error = "the fenced-transport fixture did not run";
				return false;
			}
			if (leaves != 0 || state != NetLockstepState::Running) {
				*error = "a fenced transport's disconnect was adjudicated as a leave";
				return false;
			}
			if (holdingDuringHold) {
				*error = "a round with nobody gone reported itself as holding a seat";
				return false;
			}
			if (stillUsesDeadTransport) {
				*error = "the round kept naming a superseded incarnation's transport";
				return false;
			}
			return true;
		}

		// The host's silence budget and the seat hold answer different questions, and a round that
		// loses every remote is where the two meet: adjudication decides whether to keep WAITING for a
		// peer, the hold decides whether the round may END because nobody is coming back. A peer the
		// host called gone on its own budget still holds its seat.
		bool TestCoordinatorAdjudicatedPeerKeepsItsSeat(std::string* error) {
			StarFixture fx;
			const uint32_t timeoutMs = 400;
			SeatStateStub stub;
			stub.held = true;
			stub.heldPeerId = 3; // Only the peer that wedges is holding a ticket.
			if (!fx.Start(43016, 0x7000000000000016ULL, timeoutMs, error)) {
				return false;
			}
			fx.host.SetSeatStateSource(&QuerySeatStateStub, &stub);
			if (!fx.Drive(true, true, true, [&] { return fx.Running(); }, 2000)) {
				*error = "adjudicated-seat fixture did not reach Running";
				return false;
			}
			std::vector<uint64_t> hostReady;
			if (!fx.DriveProducing(true, [&] {
					fx.Collect(fx.host, hostReady);
					return hostReady.size() >= 2;
				}, fx.now + timeoutMs)) {
				*error = "adjudicated-seat fixture did not get the round moving";
				return false;
			}
			// B wedges: the host calls it gone on its own budget rather than waiting on its socket.
			if (!fx.DriveProducing(false, [&] { return fx.host.GetPeerLeaveFrames().count(3) != 0; }, fx.now + 4 * timeoutMs)) {
				*error = "the host kept waiting for the wedged peer: " + fx.host.BuildReportJson();
				return false;
			}
			if (fx.host.GetStats().peersDroppedSilent != 1) {
				*error = "the wedged peer was not adjudicated: " + fx.host.BuildReportJson();
				return false;
			}
			// A's socket goes away too, so no remote is left - and the round must still play on,
			// because the peer the host adjudicated is holding a seat someone can come back to.
			fx.clientAT.Stop();
			const size_t committedBefore = hostReady.size();
			if (!fx.DriveProducing(false, [&] {
					fx.Collect(fx.host, hostReady);
					return hostReady.size() >= committedBefore + 4;
				}, fx.now + 4 * timeoutMs)) {
				*error = "the host stopped producing once every remote was gone: " + fx.host.BuildReportJson();
				return false;
			}
			if (!fx.host.IsRunning()) {
				*error = "a round holding an adjudicated peer's seat ended with the last remote: " + fx.host.GetStats().timeoutReason;
				return false;
			}
			// The window closes: the next tick ends a round nobody is coming back to.
			stub.held = false;
			fx.host.Tick(fx.now + 5);
			if (fx.host.GetState() != NetLockstepState::Stopped || fx.host.GetStats().timeoutReason.rfind("PeerLeft:", 0) != 0) {
				*error = "the round did not end once the reclaim window closed: " + fx.host.GetStats().timeoutReason;
				return false;
			}
			return true;
		}

		// A6: a seat inside its reclaim window still has a player, so its units keep playing under the
		// relay host instead of standing down to be shot where they stand - which is what emptied the
		// returner's team before the resync snapshot was ever taken.
		bool TestCoordinatorHeldSeatKeepsPlaying(std::string* error) {
			const uint16_t port = 43040;
			const uint64_t sessionId = 0x7000000000000040ULL;
			LoopbackTransport hostT, clientT;
			if (!hostT.StartHost(port, error) || !clientT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.timeoutMs = 5000;
				c.localPeerId = local;
				c.peerCount = 2;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				c.matchConfig.hostPeerId = 1;
				c.matchConfig.players = {{1, 0, false, "Host"}, {2, 1, false, "Client"}};
				return c;
			};
			SeatStateStub stub;
			NetLockstepCoordinator host, client;
			if (!host.Start(hostT, cfg(1, {{2, 1}}, true), error) || !client.Start(clientT, cfg(2, {{1, 1}}, false), error)) {
				return false;
			}
			host.SetSeatStateSource(&QuerySeatStateStub, &stub);
			uint64_t now = 0;
			auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
				for (const uint64_t until = now + forMs; now <= until; now += 5) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					clientT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive(2000, [&] { return host.IsRunning() && client.IsRunning(); })) {
				*error = "the held-seat ownership fixture did not reach Running";
				return false;
			}

			// A round that has committed nothing may not be judged yet: after a resync relaunch the
			// ledgered reseat rides the first committed frame.
			if (host.HasCommittedAFrame()) {
				*error = "a round reported a committed frame before it had one";
				return false;
			}
			const int64_t clientActor = 4242;
			if (host.ResolveActorOwner(clientActor, 1, false) != 2) {
				*error = "the client's team did not resolve to the client before the drop";
				return false;
			}
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !client.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error)) {
					return false;
				}
			}
			NetLockstepReadyFrame ready;
			size_t committed = 0;
			if (!drive(2000, [&] {
					while (host.PopReadyFrame(ready)) {
						++committed;
					}
					return committed >= 2;
				})) {
				*error = "the held-seat ownership fixture never committed a frame";
				return false;
			}
			if (!host.HasCommittedAFrame()) {
				*error = "a round that committed two frames still reported none";
				return false;
			}

			// The drop, with the seat held: the client's units become the host's to play, and nothing
			// stands them down.
			stub.held = true;
			clientT.Stop();
			drive(200, [] { return false; });
			if (host.GetPeerLeaveFrames().size() != 1 || !host.IsHoldingSeatForReclaim()) {
				*error = "the held drop did not put the round in its reclaim window";
				return false;
			}
			if (host.ResolveActorOwner(clientActor, 1, false) != 1) {
				*error = "a held seat's units did not fall to the relay host";
				return false;
			}
			if (host.IsActorOwnerGone(clientActor, 1, false, host.GetStats().nextFrame)) {
				*error = "a held seat's units were stood down while their player could still return";
				return false;
			}
			if (!host.IsLocalActor(clientActor, 1, false)) {
				*error = "the relay host did not take the held seat's units as its own";
				return false;
			}
			// The host's own units are untouched by any of this.
			if (host.ResolveActorOwner(7777, 0, false) != 1 || host.IsActorOwnerGone(7777, 0, false, host.GetStats().nextFrame)) {
				*error = "the hold moved the host's own units";
				return false;
			}

			// The control: once the window closes the seat has no player, and the units stand down
			// exactly as they did before - which is the pre-A6 behaviour, kept. The round reads the seat
			// on its tick, so that is where the closed window lands.
			stub.held = false;
			host.Tick(now + 5);
			if (host.ResolveActorOwner(clientActor, 1, false) != 0) {
				*error = "a released seat's units still had an owner";
				return false;
			}
			if (!host.IsActorOwnerGone(clientActor, 1, false, host.GetStats().nextFrame)) {
				*error = "a released seat's units were not stood down";
				return false;
			}
			return true;
		}

		// The match service holds its own mutex across the whole session pump, and the drop's ownership
		// census runs inside it - so a seat read from an ownership query locks that mutex on the thread
		// that already owns it. This raises what MSVC's std::mutex raises there instead of re-locking it,
		// which is undefined rather than observable.
		struct ServiceLockedSeatStateStub {
			SeatStateStub seat;
			std::mutex mutex;
			std::thread::id owner;
			uint32_t reads = 0;
			uint32_t reentries = 0;
		};

		// Stands in for NetMatchService::PumpSessionEvents holding m_Mutex for the length of the pump.
		struct ServiceLockScope {
			explicit ServiceLockScope(ServiceLockedSeatStateStub& stub) :
			    m_Stub(stub), m_Lock(stub.mutex) { m_Stub.owner = std::this_thread::get_id(); }
			~ServiceLockScope() { m_Stub.owner = std::thread::id{}; }
			ServiceLockedSeatStateStub& m_Stub;
			std::lock_guard<std::mutex> m_Lock;
		};

		NetLockstepSeatState QuerySeatStateUnderServiceLock(void* context, uint8_t peerId, NetPeerId transportPeerId) {
			auto* stub = static_cast<ServiceLockedSeatStateStub*>(context);
			if (stub->owner == std::this_thread::get_id()) {
				++stub->reentries;
				throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur), "seat state read under the match service's lock");
			}
			const std::lock_guard<std::mutex> lock(stub->mutex);
			++stub->reads;
			return QuerySeatStateStub(&stub->seat, peerId, transportPeerId);
		}

		// A6: the drop of the last remote with nobody left on its team is the branch that asks whether the
		// seat is held - and on the host it is asked from inside the pump that holds the service's lock.
		// The round must answer that from what it already knows, never by asking back.
		bool TestSeatStateNeverReadUnderTheServiceLock(std::string* error) {
			const uint16_t port = 43042;
			const uint64_t sessionId = 0x7000000000000042ULL;
			LoopbackTransport hostT, clientT;
			if (!hostT.StartHost(port, error) || !clientT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.timeoutMs = 200;
				c.localPeerId = local;
				c.peerCount = 2;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				c.matchConfig.hostPeerId = 1;
				c.matchConfig.players = {{1, 0, false, "Host"}, {2, 1, false, "Client"}};
				return c;
			};
			ServiceLockedSeatStateStub stub;
			stub.seat.held = true;
			NetLockstepCoordinator host, client;
			if (!host.Start(hostT, cfg(1, {{2, 1}}, true), error) || !client.Start(clientT, cfg(2, {{1, 1}}, false), error)) {
				return false;
			}
			host.SetSeatStateSource(&QuerySeatStateUnderServiceLock, &stub);
			uint64_t now = 0;
			auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
				for (const uint64_t until = now + forMs; now <= until; now += 5) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					clientT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive(2000, [&] { return host.IsRunning() && client.IsRunning(); })) {
				*error = "the service-lock fixture did not reach Running";
				return false;
			}
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !client.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error)) {
					return false;
				}
			}
			NetLockstepReadyFrame ready;
			size_t committed = 0;
			if (!drive(2000, [&] {
					while (host.PopReadyFrame(ready)) {
						++committed;
					}
					return committed >= 2;
				})) {
				*error = "the service-lock fixture never committed a frame";
				return false;
			}
			// The drop: the only remote goes away and its team has no other human, so the ownership
			// fallback has to decide whether the seat is held.
			clientT.Stop();
			if (!drive(600, [&] { return host.GetPeerLeaveFrames().count(2) != 0; })) {
				*error = "the drop was never adjudicated as a leave";
				return false;
			}
			if (!host.IsHoldingSeatForReclaim()) {
				*error = "the held drop did not put the round in its reclaim window";
				return false;
			}
			for (uint64_t f = 2; f < 5; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error)) {
					return false;
				}
			}

			const int64_t clientActor = 4242;
			uint32_t pumps = 0;
			uint8_t censusOwner = 0;
			bool censusOwnerGone = true;
			bool censusLocal = false;
			bool censusHolding = false;
			// The census the drop takes, run from inside the service's critical section exactly as
			// PumpSessionEvents runs it - and driven from the production wait loop, not a hand-rolled one.
			ScenarioRunner::SetLockstepCoordinator(&host);
			ScenarioRunner::SetSessionPump([&] {
				const ServiceLockScope serviceLock(stub);
				++pumps;
				censusOwner = host.ResolveActorOwner(clientActor, 1, false);
				censusOwnerGone = host.IsActorOwnerGone(clientActor, 1, false, host.GetStats().nextFrame);
				censusLocal = host.IsLocalActor(clientActor, 1, false);
				censusHolding = host.IsHoldingSeatForReclaim();
			});
			std::string reentry;
			try {
				for (uint64_t f = 2; f < 5; ++f) {
					std::string waitError;
					if (!ScenarioRunner::WaitForLockstepControllerFrame(f, ready, &waitError)) {
						break;
					}
				}
			} catch (const std::system_error& fault) {
				reentry = fault.what();
			}
			ScenarioRunner::SetSessionPump(nullptr);
			ScenarioRunner::SetLockstepCoordinator(nullptr);

			if (!reentry.empty() || stub.reentries != 0) {
				*error = "the drop's ownership census read the seat state under the match service's lock: " + reentry;
				return false;
			}
			if (pumps == 0) {
				*error = "the census never ran inside the service's lock";
				return false;
			}
			if (stub.reads == 0) {
				*error = "the round never read the seat state at all";
				return false;
			}
			// A6's semantics, seen from where the host actually asks: the held seat's units are the
			// host's to play and nothing stands them down.
			if (censusOwner != 1 || censusOwnerGone || !censusLocal || !censusHolding) {
				*error = "the census did not see the held seat's units fall to the relay host";
				return false;
			}
			return true;
		}

		// A6: the sim thread parks in the lockstep wait while a peer is silent, so the admission plane
		// has to be serviced from inside it - the silent peer may be waiting on the very answer only
		// that pump can send, which is what left every clean leave unacknowledged.
		// Four peers over a host-star loopback, each reporting N changing sound readings a frame, so the
		// relay's own byte counters say what the compact form costs. The same frames priced the way
		// version 13 spelled them out give the factor. The first frame and the steady ones are measured
		// apart, because only the first spells its keys out.
		bool TestFourPeerObservationRelayBytes(std::string* error) {
			for (const size_t observationsPerFrame: {size_t{64}, size_t{256}, size_t{512}}) {
				const uint16_t port = static_cast<uint16_t>(43040 + observationsPerFrame % 16);
				const uint64_t sessionId = 0x7000000000000040ULL + observationsPerFrame;
				LoopbackTransport hostT, clientAT, clientBT, clientCT;
				if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) ||
				    !clientBT.Connect("loopback", port, error) || !clientCT.Connect("loopback", port, error)) {
					return false;
				}
				auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
					NetLockstepConfig c;
					c.sessionId = sessionId;
					c.startFrame = 0;
					c.inputDelayFrames = 0;
					c.timeoutMs = 4000;
					c.localPeerId = local;
					c.peerCount = 4;
					c.remoteTransportPeerIds = std::move(transports);
					c.relayToOtherPeers = relay;
					c.frameLane = NetTransportLane::ControlReliable;
					c.scenario = "LockstepSelfTest";
					c.ownershipPolicy = "unique-id-split";
					c.roundId = 0x1400000000000001ULL + observationsPerFrame;
					return c;
				};
				NetLockstepCoordinator host, clientA, clientB, clientC;
				NetLockstepConfig clientCfg = cfg(2, {{1, 1}}, false);
				clientCfg.roundId = 0;
				if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}, {4, 3}}, true), error)) {
					return false;
				}
				clientCfg.localPeerId = 2;
				if (!clientA.Start(clientAT, clientCfg, error)) {
					return false;
				}
				clientCfg.localPeerId = 3;
				if (!clientB.Start(clientBT, clientCfg, error)) {
					return false;
				}
				clientCfg.localPeerId = 4;
				if (!clientC.Start(clientCT, clientCfg, error)) {
					return false;
				}
				NetLockstepCoordinator* peers[4] = {&host, &clientA, &clientB, &clientC};
				LoopbackTransport* transports[4] = {&hostT, &clientAT, &clientBT, &clientCT};
				auto drive = [&](const std::function<bool()>& done) {
					for (uint64_t now = 0; now <= 20000; now += 5) {
						for (NetLockstepCoordinator* peer: peers) {
							peer->Tick(now);
						}
						if (done()) {
							return true;
						}
						for (LoopbackTransport* transport: transports) {
							transport->AdvanceTimeMs(5);
						}
					}
					return false;
				};
				if (!drive([&] { return host.IsRunning() && clientA.IsRunning() && clientB.IsRunning() && clientC.IsRunning(); })) {
					*error = "four-peer lockstep did not reach Running";
					return false;
				}

				std::map<uint8_t, std::map<uint64_t, std::vector<NetSoundObservation>>> committed;
				size_t readyPerPeer[4] = {0, 0, 0, 0};
				auto collect = [&](size_t index) {
					NetLockstepReadyFrame ready;
					while (peers[index]->PopReadyFrame(ready)) {
						std::vector<NetSoundObservation> all = ready.localObservations;
						all.insert(all.end(), ready.remoteObservations.begin(), ready.remoteObservations.end());
						std::stable_sort(all.begin(), all.end(), [](const NetSoundObservation& a, const NetSoundObservation& b) {
							return std::tie(a.senderPeerId, a.objectUID, a.ordinal) < std::tie(b.senderPeerId, b.objectUID, b.ordinal);
						});
						committed[static_cast<uint8_t>(index)][ready.frame] = std::move(all);
						++readyPerPeer[index];
					}
				};
				// Version 13 priced the same frames at forty-four bytes an observation, on top of a packet
				// that is otherwise byte for byte the same one this build sends.
				size_t legacyBytesPerFrame = 0;
				const size_t frameCount = 12;
				const auto runFrames = [&](uint64_t from, uint64_t to) {
					for (uint64_t f = from; f < to; ++f) {
						for (uint8_t peer = 1; peer <= 4; ++peer) {
							std::vector<NetSoundObservation> observations = MakeObservationSet(peer, observationsPerFrame, 400 + peer, static_cast<float>(f) / 16.0F);
							if (peer == 1 && legacyBytesPerFrame == 0) {
								NetLockstepFrame priced;
								priced.senderPeerId = peer;
								priced.targetFrame = f;
								priced.roundId = 0x1400000000000001ULL + observationsPerFrame;
								priced.frames = {MakeFrame(100 + static_cast<int64_t>(f), f + 1)};
								priced.observations = observations;
								// Less the binding sequence of an empty block, which version 13 did not have.
								legacyBytesPerFrame = FrameBytesWithoutObservations(priced) - 1 + 44 * observationsPerFrame;
							}
							if (!peers[peer - 1]->QueueLocalInput(f, {MakeFrame(100 * peer + static_cast<int64_t>(f), f + 1)}, {}, error, observations)) {
								return false;
							}
						}
					}
					if (!drive([&] {
							for (size_t i = 0; i < 4; ++i) {
								collect(i);
							}
							return readyPerPeer[0] >= to && readyPerPeer[1] >= to && readyPerPeer[2] >= to && readyPerPeer[3] >= to;
						})) {
						*error = "four-peer lockstep did not produce every ready frame: " + std::to_string(readyPerPeer[0]) + "," + std::to_string(readyPerPeer[1]) +
						         "," + std::to_string(readyPerPeer[2]) + "," + std::to_string(readyPerPeer[3]);
						return false;
					}
					return true;
				};
				const uint64_t startBytes = host.GetStats().relayBytesSent;
				const uint32_t startPackets = host.GetStats().relayPacketsSent;
				if (!runFrames(0, 1)) {
					return false;
				}
				const uint64_t firstUseBytes = host.GetStats().relayBytesSent - startBytes;
				const uint32_t firstUsePackets = host.GetStats().relayPacketsSent - startPackets;
				if (!runFrames(1, frameCount)) {
					return false;
				}
				const uint64_t steadyBytes = host.GetStats().relayBytesSent - startBytes - firstUseBytes;
				const uint32_t steadyPackets = host.GetStats().relayPacketsSent - startPackets - firstUsePackets;

				// Every peer commits the identical table, which is the whole point of the wire form.
				for (uint64_t f = 0; f < frameCount; ++f) {
					for (uint8_t peer = 1; peer < 4; ++peer) {
						if (committed[peer][f] != committed[0][f]) {
							*error = "four-peer observation tables differ at frame " + std::to_string(f) + " on peer " + std::to_string(peer + 1);
							return false;
						}
					}
					if (committed[0][f].size() != observationsPerFrame * 4) {
						*error = "four-peer frame " + std::to_string(f) + " committed " + std::to_string(committed[0][f].size()) +
						         " observations, expected " + std::to_string(observationsPerFrame * 4);
						return false;
					}
				}
				const NetLockstepStats& stats = host.GetStats();
				if (stats.relayObservationOverflows != 0 || stats.unresolvedObservationPackets != 0 ||
				    stats.observationsCarried != 0 || stats.observationsDropped != 0) {
					*error = "four-peer relay reported an observation fault";
					return false;
				}
				if (firstUsePackets == 0 || steadyPackets == 0) {
					*error = "four-peer relay forwarded nothing to measure";
					return false;
				}
				const double firstUsePerPacket = static_cast<double>(firstUseBytes) / firstUsePackets;
				const double steadyPerPacket = static_cast<double>(steadyBytes) / steadyPackets;
				const double legacy = static_cast<double>(legacyBytesPerFrame);
				std::cout << "[net-lockstep-selftest] PASS four_peer_observation_relay n=" << observationsPerFrame
				          << " relayed_bytes_per_frame first_use=" << firstUsePerPacket << " steady=" << steadyPerPacket
				          << " (was " << legacy << ") factor first_use=" << legacy / firstUsePerPacket << " steady=" << legacy / steadyPerPacket
				          << " relay_bytes_sent=" << stats.relayBytesSent << " largest_relay_packet_bytes=" << stats.largestRelayPacketBytes << std::endl;
				if (legacy / steadyPerPacket < 6.0) {
					*error = "the compact observation form saved less than six times in the steady state at n=" + std::to_string(observationsPerFrame);
					return false;
				}
			}
			return true;
		}

		// More changed readings in one frame than its byte budget holds: the sender keeps the rest for the
		// next frame instead of refusing the frame, and both peers commit the same table either way.
		bool TestObservationOverflowCarry(std::string* error) {
			LoopbackTransport hostTransport, clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, 0x7000000000000050ULL, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, 0x7000000000000050ULL, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = 0x1400000000000050ULL;
			hostConfig.timeoutMs = 4000;
			clientConfig.timeoutMs = 4000;
			if (!StartCoordinatorPair(43060, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error, 8000)) {
				return false;
			}
			// Every key is new every frame, so the block is nothing but spelled-out keys and the budget bites.
			uint64_t nextObject = 30000000;
			const auto flood = [&](size_t count, uint64_t tick) {
				std::vector<NetSoundObservation> observations;
				for (size_t i = 0; i < count; ++i) {
					observations.push_back(MakeObservation(1, nextObject++, tick, 0xF0E1D2C3B4A59687ULL + nextObject, 1 + i % 4, 0.125F));
				}
				return observations;
			};
			// Two frames of new sounds nobody can hold, then quiet ones for the backlog to drain into.
			const size_t burst = 2048;
			const size_t frameCount = 10;
			std::vector<NetSoundObservation> sent;
			for (uint64_t f = 0; f < frameCount; ++f) {
				std::vector<NetSoundObservation> observations = f < 2 ? flood(burst, 700 + f) : std::vector<NetSoundObservation>{};
				sent.insert(sent.end(), observations.begin(), observations.end());
				if (!host.QueueLocalInput(f, {MakeFrame(100 + static_cast<int64_t>(f), f + 1)}, {}, error, observations) ||
				    !client.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error)) {
					return false;
				}
			}
			std::vector<NetSoundObservation> hostSeen, clientSeen;
			size_t hostReady = 0, clientReady = 0;
			auto collect = [&](NetLockstepCoordinator& coordinator, std::vector<NetSoundObservation>& into, size_t& count) {
				NetLockstepReadyFrame ready;
				while (coordinator.PopReadyFrame(ready)) {
					into.insert(into.end(), ready.localObservations.begin(), ready.localObservations.end());
					into.insert(into.end(), ready.remoteObservations.begin(), ready.remoteObservations.end());
					++count;
				}
			};
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] {
					collect(host, hostSeen, hostReady);
					collect(client, clientSeen, clientReady);
					return hostReady >= frameCount && clientReady >= frameCount;
				}, error, 16000)) {
				return false;
			}
			if (host.GetStats().observationsCarried == 0) {
				*error = "a frame of nothing but new keys did not carry anything over";
				return false;
			}
			if (host.GetStats().observationsDropped != 0) {
				*error = "a burst the quiet frames could drain still dropped readings";
				return false;
			}
			// The two peers saw the same readings in the same order, and the burst arrived whole.
			if (hostSeen != clientSeen) {
				*error = "the carried observations reached the two peers differently";
				return false;
			}
			if (hostSeen != sent) {
				*error = "the carried observations did not all arrive in their sampled order: " +
				         std::to_string(hostSeen.size()) + " of " + std::to_string(sent.size());
				return false;
			}
			const uint64_t carriedInBurst = host.GetStats().observationsCarried;

			// Sustained: more new sounds every frame than the wire can ever carry. The held set stays
			// bounded, the oldest readings go, and the frames themselves keep flowing.
			for (uint64_t f = frameCount; f < frameCount + 8; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100 + static_cast<int64_t>(f), f + 1)}, {}, error, flood(NetLockstepCodec::c_MaxObservationsPerPacket, 800 + f)) ||
				    !client.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error)) {
					return false;
				}
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] {
					collect(host, hostSeen, hostReady);
					collect(client, clientSeen, clientReady);
					return hostReady >= frameCount + 8 && clientReady >= frameCount + 8;
				}, error, 16000)) {
				return false;
			}
			if (host.GetStats().observationsDropped == 0) {
				*error = "the held observation set was not bounded under a sustained flood";
				return false;
			}
			// Every dropped reading comes back exactly once, in the order it was sampled, so its sampler
			// can forget it was ever sent and offer it again.
			const std::vector<NetSoundObservation> handedBack = host.TakeDroppedObservations();
			if (handedBack.size() != host.GetStats().observationsDropped) {
				*error = "the dropped readings were not all handed back: " + std::to_string(handedBack.size()) +
				         " of " + std::to_string(host.GetStats().observationsDropped);
				return false;
			}
			std::set<NetSoundObservationKey> committedKeys;
			for (const NetSoundObservation& observation: hostSeen) {
				committedKeys.insert(KeyOfObservation(observation));
			}
			for (const NetSoundObservation& observation: handedBack) {
				if (committedKeys.contains(KeyOfObservation(observation))) {
					*error = "a reading was both committed and handed back as dropped";
					return false;
				}
			}
			if (!host.TakeDroppedObservations().empty()) {
				*error = "a dropped reading was handed back twice";
				return false;
			}
			if (hostSeen != clientSeen) {
				*error = "a bounded held set left the two peers with different observations";
				return false;
			}
			if (host.IsFailed() || client.IsFailed()) {
				*error = "driving past the observation cap failed the round";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS observation_overflow_carry burst=" << burst << " carried=" << carriedInBurst
			          << " delivered=" << sent.size() << "/" << sent.size() << " sustained_dropped=" << host.GetStats().observationsDropped << std::endl;
			return true;
		}

		// A frame from another lockstep round is the round's business, not the codec's: it decodes like
		// any other, its sender still counts as heard from, and the round drops it.
		bool TestStaleRoundFrameStillCountsAsTraffic(std::string* error) {
			LoopbackTransport hostTransport, clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, 0x7000000000000070ULL, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, 0x7000000000000070ULL, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = 0x1400000000000070ULL;
			hostConfig.timeoutMs = 4000;
			clientConfig.timeoutMs = 4000;
			if (!StartCoordinatorPair(43070, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error, 4000)) {
				return false;
			}
			const NetLockstepPeerStats before = host.GetStats().peers.at(2);
			const uint32_t staleBefore = host.GetStats().staleRoundPackets;

			// A frame of the round before this one, with observations whose slots this host was never given.
			NetSoundObservationDictionary strangerEncoder;
			NetLockstepFrame stale;
			stale.senderPeerId = 2;
			stale.targetFrame = 4;
			stale.roundId = hostConfig.roundId - 1;
			stale.frames = {MakeFrame(300, 1)};
			stale.observations = MakeObservationSet(2, 6, 55, 0.0F);
			std::vector<uint8_t> bytes;
			size_t encoded = 0;
			if (!NetLockstepCodec::Encode({stale}, bytes, nullptr, &strangerEncoder, &encoded) || encoded != stale.observations.size()) {
				*error = "the stale-round frame did not encode";
				return false;
			}
			if (!clientTransport.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			for (uint64_t now = 5000; now <= 5100; now += 5) {
				host.Tick(now);
				hostTransport.AdvanceTimeMs(5);
				clientTransport.AdvanceTimeMs(5);
			}
			const NetLockstepPeerStats after = host.GetStats().peers.at(2);
			if (after.staleRoundPackets != before.staleRoundPackets + 1 || host.GetStats().staleRoundPackets != staleBefore + 1) {
				*error = "a stale-round frame was not counted against its sender";
				return false;
			}
			if (after.framePacketsReceived != before.framePacketsReceived + 1) {
				*error = "a stale-round frame did not count as a frame packet from its sender";
				return false;
			}
			if (after.lastHeardMs <= before.lastHeardMs) {
				*error = "a stale-round frame did not prove its sender is still there";
				return false;
			}
			if (host.GetStats().unresolvedObservationPackets != 0 || host.IsFailed()) {
				*error = "a stale-round frame disturbed the round";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS stale_round_frame_counts_as_traffic last_heard=" << before.lastHeardMs << "->" << after.lastHeardMs
			          << " stale=" << after.staleRoundPackets << " unresolved=0" << std::endl;
			return true;
		}

		// Two frames a relay host cannot read the observations of, for opposite reasons: one from a
		// transport that is not in the round at all, one from a peer that is. Only the second is a fault.
		bool TestObservationFaultsAreToldApart(std::string* error) {
			const uint16_t port = 43080;
			const uint64_t sessionId = 0x7000000000000080ULL;
			LoopbackTransport hostT, clientAT, clientBT, strangerT;
			if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) ||
			    !clientBT.Connect("loopback", port, error) || !strangerT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.startFrame = 0;
				c.inputDelayFrames = 0;
				c.timeoutMs = 4000;
				c.localPeerId = local;
				c.peerCount = 3;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.frameLane = NetTransportLane::ControlReliable;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				c.roundId = 0x1400000000000080ULL;
				return c;
			};
			NetLockstepCoordinator host, clientA, clientB;
			NetLockstepConfig clientCfg = cfg(2, {{1, 1}}, false);
			clientCfg.roundId = 0;
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error)) {
				return false;
			}
			clientCfg.localPeerId = 2;
			if (!clientA.Start(clientAT, clientCfg, error)) {
				return false;
			}
			clientCfg.localPeerId = 3;
			if (!clientB.Start(clientBT, clientCfg, error)) {
				return false;
			}
			NetLockstepCoordinator* peers[3] = {&host, &clientA, &clientB};
			LoopbackTransport* transports[4] = {&hostT, &clientAT, &clientBT, &strangerT};
			auto drive = [&](uint64_t from, uint64_t to) {
				for (uint64_t now = from; now <= to; now += 5) {
					for (NetLockstepCoordinator* peer: peers) {
						peer->Tick(now);
					}
					for (LoopbackTransport* transport: transports) {
						transport->AdvanceTimeMs(5);
					}
				}
			};
			drive(0, 1000);
			if (!host.IsRunning()) {
				*error = "the fault-classification host did not reach Running";
				return false;
			}

			// A frame whose observations refer to slots this host was never given.
			NetSoundObservationDictionary strangerEncoder;
			std::vector<uint8_t> binding, reference;
			size_t encoded = 0;
			if (!NetLockstepCodec::Encode({MakeObservationFrame(2, 0x1400000000000080ULL, MakeObservationSet(2, 4, 31, 0.0F))}, binding, nullptr, &strangerEncoder, &encoded) ||
			    !NetLockstepCodec::Encode({MakeObservationFrame(3, 0x1400000000000080ULL, MakeObservationSet(2, 4, 31, 0.5F))}, reference, nullptr, &strangerEncoder, &encoded)) {
				*error = "the unreadable frames did not encode";
				return false;
			}

			// From a transport with no seat in the round: admission traffic, not a fault of ours.
			const uint32_t admissionBefore = host.GetStats().ignoredAdmissionFaults;
			if (!strangerT.Send(1, NetTransportLane::ControlReliable, reference, error)) {
				return false;
			}
			drive(1005, 1200);
			if (host.GetStats().ignoredAdmissionFaults != admissionBefore + 1 || host.GetStats().unresolvedObservationPackets != 0) {
				*error = "an unknown transport's unreadable frame was counted as a fault: admission=" +
				         std::to_string(host.GetStats().ignoredAdmissionFaults) + " unresolved=" + std::to_string(host.GetStats().unresolvedObservationPackets);
				return false;
			}

			// From a peer of the round, the same shape means a binding it can never be told again.
			if (!clientAT.Send(1, NetTransportLane::ControlReliable, reference, error)) {
				return false;
			}
			drive(1205, 1400);
			if (host.GetStats().unresolvedObservationPackets != 1) {
				*error = "a round member's unreadable frame was not counted as a fault";
				return false;
			}
			if (host.IsFailed() || !host.IsRunning()) {
				*error = "an unreadable observation block ended the round";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS observation_faults_told_apart admission=" << host.GetStats().ignoredAdmissionFaults
			          << " unresolved=" << host.GetStats().unresolvedObservationPackets << std::endl;
			return true;
		}

		// A frame the round is going to discard must not disturb the live table of the peer that sent it,
		// whether its sender's encoder is fresh or ahead. The reviewer's stale_round_shape probe.
		bool TestStaleRoundFrameLeavesTheLiveTable(std::string* error) {
			for (int shape = 0; shape < 2; ++shape) {
				LoopbackTransport hostTransport, clientTransport;
				NetLockstepCoordinator host, client;
				const uint64_t sessionId = 0x7000000000000090ULL + static_cast<uint64_t>(shape);
				NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::ControlReliable);
				NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::ControlReliable);
				hostConfig.roundId = 0x1400000000000090ULL;
				hostConfig.timeoutMs = 8000;
				clientConfig.timeoutMs = 8000;
				if (!StartCoordinatorPair(static_cast<uint16_t>(43090 + shape), hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
					return false;
				}
				// One clock for the whole probe, so the peer's last-heard stamp can be read as it moves.
				uint64_t clock = 0;
				size_t hostReady = 0;
				std::vector<NetSoundObservation> hostSaw;
				auto drive = [&](const std::function<bool()>& done) {
					for (uint64_t step = 0; step < 2000; ++step) {
						// The clock moves before every tick, so a peer's last-heard stamp reads as it moves.
						hostTransport.AdvanceTimeMs(5);
						clientTransport.AdvanceTimeMs(5);
						clock += 5;
						host.Tick(clock);
						client.Tick(clock);
						NetLockstepReadyFrame ready;
						while (host.PopReadyFrame(ready)) {
							hostSaw = ready.remoteObservations;
							++hostReady;
						}
						if (done()) {
							return true;
						}
					}
					return false;
				};
				if (!drive([&] { return host.IsRunning() && client.IsRunning(); })) {
					*error = "the straggler probe did not reach Running";
					return false;
				}
				// Real traffic first, so the host's table for peer 2 is not empty when the straggler lands.
				const auto play = [&](uint64_t from, uint64_t to) {
					for (uint64_t f = from; f < to; ++f) {
						if (!host.QueueLocalInput(f, {MakeFrame(100 + static_cast<int64_t>(f), f + 1)}, {}, error) ||
						    !client.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error, MakeObservationSet(2, 12, 61, static_cast<float>(f) / 8.0F))) {
							return false;
						}
					}
					return drive([&] { return hostReady >= to; });
				};
				if (!play(0, 3)) {
					if (error->empty()) { *error = "the straggler probe's opening frames did not commit"; }
					return false;
				}
				const NetLockstepPeerStats before = host.GetStats().peers.at(2);
				const uint32_t unresolvedBefore = host.GetStats().unresolvedObservationPackets;

				// A frame of the previous round. Shape 0 comes from a fresh encoder, which is the zero count
				// that would reset a live table; shape 1 comes from one that is ahead of the host's.
				NetSoundObservationDictionary strangerEncoder;
				size_t encoded = 0;
				if (shape == 1) {
					for (int warm = 0; warm < 3; ++warm) {
						std::vector<uint8_t> scratch;
						if (!NetLockstepCodec::Encode({MakeObservationFrame(90 + warm, hostConfig.roundId - 1, MakeObservationSet(2, 9, 300 + warm, 0.0F))}, scratch, nullptr, &strangerEncoder, &encoded)) {
							*error = "the straggler's warm-up did not encode";
							return false;
						}
					}
				}
				NetLockstepFrame stale;
				stale.senderPeerId = 2;
				stale.targetFrame = 3;
				stale.roundId = hostConfig.roundId - 1;
				stale.frames = {MakeFrame(999, 1)};
				stale.observations = MakeObservationSet(2, 6, 400, 0.25F);
				std::vector<uint8_t> bytes;
				if (!NetLockstepCodec::Encode({stale}, bytes, nullptr, &strangerEncoder, &encoded) || encoded != stale.observations.size()) {
					*error = "the stale-round frame did not encode";
					return false;
				}
				if (!clientTransport.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
					return false;
				}
				if (!drive([&] { return host.GetStats().peers.at(2).staleRoundPackets > before.staleRoundPackets; })) {
					*error = "shape " + std::to_string(shape) + ": the straggler never reached the round";
					return false;
				}
				const NetLockstepPeerStats after = host.GetStats().peers.at(2);
				if (after.staleRoundPackets != before.staleRoundPackets + 1 || after.framePacketsReceived != before.framePacketsReceived + 1) {
					*error = "shape " + std::to_string(shape) + ": the straggler was not counted against its sender";
					return false;
				}
				if (after.lastHeardMs <= before.lastHeardMs) {
					*error = "shape " + std::to_string(shape) + ": the straggler did not prove its sender is still there (" +
					         std::to_string(before.lastHeardMs) + " -> " + std::to_string(after.lastHeardMs) + ")";
					return false;
				}
				if (host.GetStats().unresolvedObservationPackets != unresolvedBefore) {
					*error = "shape " + std::to_string(shape) + ": a discarded straggler was counted as a hole in its sender's stream";
					return false;
				}

				// The live table has to have survived: the client's next frames are mostly slot references.
				if (!play(3, 5)) {
					*error = "shape " + std::to_string(shape) + ": the peer was muted after the straggler";
					return false;
				}
				if (hostReady < 5 || hostSaw.size() != 12) {
					*error = "shape " + std::to_string(shape) + ": the host committed " + std::to_string(hostReady) +
					         " frames and " + std::to_string(hostSaw.size()) + " observations after the straggler";
					return false;
				}
				if (host.GetStats().unresolvedObservationPackets != unresolvedBefore || host.IsFailed()) {
					*error = "shape " + std::to_string(shape) + ": the round did not survive the straggler";
					return false;
				}
				std::cout << "[net-lockstep-selftest] PASS stale_round_leaves_the_live_table shape=" << shape
				          << " stale=" << after.staleRoundPackets << " unresolved=" << host.GetStats().unresolvedObservationPackets
				          << " last_heard=" << before.lastHeardMs << "->" << after.lastHeardMs
				          << " frames=" << hostReady << " observations=" << hostSaw.size() << std::endl;
			}
			return true;
		}

		// A resync restarts the host while the other peer is still producing frames of the old round. Those
		// are ordinary stragglers, not holes. The reviewer's resync_straggler_counter probe.
		bool TestResyncStragglersAreNotHoles(std::string* error) {
			LoopbackTransport hostTransport, clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, 0x70000000000000A0ULL, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, 0x70000000000000A0ULL, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = 0x14000000000000A0ULL;
			hostConfig.timeoutMs = 20000;
			clientConfig.timeoutMs = 20000;
			if (!StartCoordinatorPair(43092, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			uint64_t clock = 0;
			size_t hostReady = 0;
			auto drive = [&](const std::function<bool()>& done) {
				for (uint64_t step = 0; step < 3000; ++step) {
					// The clock moves before every tick, so a peer's last-heard stamp reads as it moves.
					hostTransport.AdvanceTimeMs(5);
					clientTransport.AdvanceTimeMs(5);
					clock += 5;
					host.Tick(clock);
					client.Tick(clock);
					NetLockstepReadyFrame ready;
					while (host.PopReadyFrame(ready)) {
						++hostReady;
					}
					if (done()) {
						return true;
					}
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); })) {
				*error = "the resync probe did not reach Running";
				return false;
			}
			for (uint64_t f = 0; f < 3; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100 + static_cast<int64_t>(f), f + 1)}, {}, error) ||
				    !client.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error, MakeObservationSet(2, 10, 71, static_cast<float>(f) / 8.0F))) {
					return false;
				}
			}
			if (!drive([&] { return hostReady >= 3; })) {
				*error = "the resync probe's opening frames did not commit";
				return false;
			}

			// The host restarts into a new round; the client has not noticed yet and keeps sending.
			NetLockstepConfig resyncConfig = hostConfig;
			resyncConfig.roundId = hostConfig.roundId + 1;
			resyncConfig.remoteTransportPeerId = 1;
			if (!host.Start(hostTransport, resyncConfig, error)) {
				return false;
			}
			for (uint64_t f = 3; f < 6; ++f) {
				if (!client.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error, MakeObservationSet(2, 10, 71, static_cast<float>(f) / 8.0F))) {
					return false;
				}
			}
			drive([&] { return host.GetStats().staleRoundPackets >= 3; });
			const NetLockstepStats& afterStragglers = host.GetStats();
			if (afterStragglers.unresolvedObservationPackets != 0) {
				*error = "a resync's in-flight frames were counted as holes: " + std::to_string(afterStragglers.unresolvedObservationPackets);
				return false;
			}
			if (afterStragglers.staleRoundPackets != 3 || afterStragglers.peers.at(2).staleRoundPackets != 3) {
				*error = "a resync's in-flight frames were not counted as stragglers: " + std::to_string(afterStragglers.staleRoundPackets);
				return false;
			}
			if (afterStragglers.peers.at(2).lastHeardMs == 0) {
				*error = "a resync's in-flight frames did not prove their sender is still there";
				return false;
			}
			const uint64_t stragglerStamp = afterStragglers.peers.at(2).lastHeardMs;

			// The client catches up; the round re-forms and runs.
			NetLockstepConfig clientResync = clientConfig;
			clientResync.remoteTransportPeerId = 1;
			if (!client.Start(clientTransport, clientResync, error)) {
				return false;
			}
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); })) {
				*error = "the round did not re-form after the resync";
				return false;
			}
			hostReady = 0;
			for (uint64_t f = 0; f < 4; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(300 + static_cast<int64_t>(f), f + 1)}, {}, error) ||
				    !client.QueueLocalInput(f, {MakeFrame(400 + static_cast<int64_t>(f), f + 1)}, {}, error, MakeObservationSet(2, 10, 81, static_cast<float>(f) / 8.0F))) {
					return false;
				}
			}
			if (!drive([&] { return hostReady >= 4; })) {
				*error = "the recovered round did not commit its frames";
				return false;
			}
			if (host.GetStats().unresolvedObservationPackets != 0) {
				*error = "the recovered round reported a hole";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS resync_stragglers_are_not_holes stale=" << host.GetStats().staleRoundPackets
			          << " unresolved=0 last_heard=" << stragglerStamp << " recovered_frames=" << hostReady << std::endl;
			return true;
		}

		// A block that fails part way through must leave the table exactly as it was, or the sender's next
		// perfectly good packet reads as a hole. The reviewer's partial_bind_then_refuse probe.
		bool TestRefusedBlockLeavesNoBindings(std::string* error) {
			NetSoundObservationDictionary sender;
			NetSoundObservationTables receiver;
			std::vector<uint8_t> first;
			size_t encoded = 0;
			if (!NetLockstepCodec::Encode({MakeObservationFrame(10, 0, MakeObservationSet(2, 6, 91, 0.0F))}, first, nullptr, &sender, &encoded) || encoded != 6) {
				*error = "the truncation probe's frame did not encode";
				return false;
			}
			// Cut the block in half and re-stamp the payload length, so the packet is well formed up to the
			// point where it runs out.
			std::vector<uint8_t> truncated(first.begin(), first.begin() + static_cast<std::ptrdiff_t>(first.size() - 40));
			for (int i = 0; i < 4; ++i) {
				truncated[12 + i] = static_cast<uint8_t>((truncated.size() - NetLockstepCodec::c_HeaderBytes) >> (i * 8));
			}
			const NetLockstepDecodeResult refused = NetLockstepCodec::Decode(truncated, ControllerFrame::c_Version, &receiver);
			if (refused.ok || refused.error.code != NetLockstepErrorCode::TruncatedPayload) {
				*error = "a truncated observation block was not refused: " + std::string(NetLockstepCodec::ErrorCodeName(refused.error.code));
				return false;
			}
			if (receiver.Exactly(2).BindingCount() != 0) {
				*error = "a refused block left " + std::to_string(receiver.Exactly(2).BindingCount()) + " bindings behind";
				return false;
			}
			// The same frame, whole, still reads, and so does the one after it.
			const NetLockstepDecodeResult whole = NetLockstepCodec::Decode(first, ControllerFrame::c_Version, &receiver);
			if (!whole.ok) {
				*error = "the whole frame did not decode after its truncated copy: " + std::string(NetLockstepCodec::ErrorCodeName(whole.error.code));
				return false;
			}
			std::vector<uint8_t> next;
			const std::vector<NetSoundObservation> repeats = MakeObservationSet(2, 6, 91, 0.5F);
			if (!NetLockstepCodec::Encode({MakeObservationFrame(11, 0, repeats)}, next, nullptr, &sender, &encoded)) {
				*error = "the frame after the truncation did not encode";
				return false;
			}
			const NetLockstepDecodeResult following = NetLockstepCodec::Decode(next, ControllerFrame::c_Version, &receiver);
			const NetLockstepFrame* followingFrame = following.ok ? std::get_if<NetLockstepFrame>(&following.packet.payload) : nullptr;
			if (!followingFrame || followingFrame->observations != repeats) {
				*error = "the sender's next packet was refused after a truncated one: " + std::string(NetLockstepCodec::ErrorCodeName(following.error.code));
				return false;
			}
			// Staging must not change which bindings land or what a slot means while the block reads: a block
			// that spells one slot out twice binds twice, and the reference between the two reads the first key.
			NetSoundObservationTables ordered;
			std::vector<uint8_t> crafted;
			if (!NetLockstepCodec::Encode({MakeObservationFrame(12, 0, {})}, crafted)) {
				*error = "the ordering probe's frame did not encode";
				return false;
			}
			crafted.resize(crafted.size() - 3); // The empty observation block: a zero binding sequence and a zero count.
			const auto appendVar = [&crafted](uint64_t value) {
				while (value >= 0x80U) {
					crafted.push_back(static_cast<uint8_t>(value) | 0x80U);
					value >>= 7;
				}
				crafted.push_back(static_cast<uint8_t>(value));
			};
			const auto appendReading = [&crafted](float value) {
				uint32_t bits = 0;
				std::memcpy(&bits, &value, sizeof(bits));
				for (int i = 0; i < 4; ++i) {
					crafted.push_back(static_cast<uint8_t>(bits >> (i * 8)));
				}
			};
			const NetSoundObservationKey firstKey{4001, 7, 0x9E3779B97F4A7C15ULL, 1, 2};
			const NetSoundObservationKey secondKey{4002, 9, 0xC2B2AE3D27D4EB4FULL, 3, 4};
			const auto appendSlotZeroKey = [&](const NetSoundObservationKey& key) {
				appendVar(1); // Slot 0, spelled out.
				crafted.push_back(0x1F);
				appendVar(key.objectUID);
				appendVar(key.tick);
				appendVar(key.phase);
				appendVar(key.occurrence);
				appendVar(key.ordinal);
			};
			appendVar(0); // This sender has spelled nothing out before the block.
			crafted.push_back(3);
			crafted.push_back(0);
			appendSlotZeroKey(firstKey);
			appendReading(0.25F);
			appendVar(0); // Slot 0 by reference, between the two keys it is bound to.
			appendReading(0.5F);
			appendSlotZeroKey(secondKey);
			appendReading(0.75F);
			for (int i = 0; i < 4; ++i) {
				crafted[12 + i] = static_cast<uint8_t>((crafted.size() - NetLockstepCodec::c_HeaderBytes) >> (i * 8));
			}
			const NetLockstepDecodeResult twice = NetLockstepCodec::Decode(crafted, ControllerFrame::c_Version, &ordered);
			const NetLockstepFrame* twiceFrame = twice.ok ? std::get_if<NetLockstepFrame>(&twice.packet.payload) : nullptr;
			if (!twiceFrame || twiceFrame->observations.size() != 3) {
				*error = "a block spelling one slot twice did not decode: " + std::string(NetLockstepCodec::ErrorCodeName(twice.error.code));
				return false;
			}
			NetSoundObservationKey settled;
			if (KeyOfObservation(twiceFrame->observations[1]) != firstKey || KeyOfObservation(twiceFrame->observations[2]) != secondKey ||
			    ordered.Exactly(2).BindingCount() != 2 || !ordered.Exactly(2).Resolve(0, settled) || settled != secondKey) {
				*error = "staged bindings did not land in the order the block spelled them";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS refused_block_leaves_no_bindings bindings_after_refusal=0 next_packet=ok"
			          << " one_slot_twice=2_bindings mid_block_reference=first_key" << std::endl;
			return true;
		}

		bool TestSessionPumpRunsWhileTheRoundWaits(std::string* error) {
			const uint16_t port = 43050;
			const uint64_t sessionId = 0x7000000000000050ULL;
			LoopbackTransport hostT, clientT;
			if (!hostT.StartHost(port, error) || !clientT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.timeoutMs = 200;
				c.localPeerId = local;
				c.peerCount = 2;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			NetLockstepCoordinator host, client;
			if (!host.Start(hostT, cfg(1, {{2, 1}}, true), error) || !client.Start(clientT, cfg(2, {{1, 1}}, false), error)) {
				return false;
			}
			for (uint64_t now = 0; now <= 2000; now += 5) {
				host.Tick(now);
				client.Tick(now);
				if (host.IsRunning() && client.IsRunning()) {
					break;
				}
				hostT.AdvanceTimeMs(5);
				clientT.AdvanceTimeMs(5);
			}
			if (!host.IsRunning()) {
				*error = "the pump fixture did not reach Running";
				return false;
			}
			// The client never sends frame 0, so the host waits for it and times out.
			if (!host.QueueLocalInput(0, {MakeFrame(100, 1)}, {}, error)) {
				return false;
			}
			uint32_t pumps = 0;
			ScenarioRunner::SetLockstepCoordinator(&host);
			ScenarioRunner::SetSessionPump([&pumps] { ++pumps; });
			NetLockstepReadyFrame ready;
			std::string waitError;
			const bool got = ScenarioRunner::WaitForLockstepControllerFrame(0, ready, &waitError);
			ScenarioRunner::SetSessionPump(nullptr);
			ScenarioRunner::SetLockstepCoordinator(nullptr);
			if (got) {
				*error = "the wait returned a frame the peer never sent";
				return false;
			}
			if (pumps == 0) {
				*error = "the admission plane was never serviced while the round waited";
				return false;
			}
			// Paced, not spun: a wait of a few hundred ms must not run the plane's clock away.
			if (pumps > 200) {
				*error = "the wait pumped the admission plane " + std::to_string(pumps) + " times, unpaced";
				return false;
			}
			return true;
		}

		// The relay host is the only route between its clients, so its own last tick is not the round's
		// end: a client one input-delay behind still needs the forwards the host is holding. Quitting
		// there took the last frames off every client that was waiting - the 4-peer lane's 179 of 180.
		bool TestRelayHostFinishesWhatItOwes(std::string* error) {
			StarFixture fx;
			const uint32_t timeoutMs = 20000; // Long enough that the unreachable bound cannot end this.
			std::vector<uint64_t> hostReady, bReady;
			if (!StartRelayRefusal(fx, 43016, 0x7000000000000016ULL, timeoutMs, hostReady, error)) {
				return false;
			}
			const uint64_t refusedFrom = fx.now;
			fx.DriveProducing(true, [&] { return fx.host.GetStats().relaySendFailures > 0; }, refusedFrom + 400);
			if (fx.host.GetStats().relaySendFailures == 0) {
				*error = "the refusal never reached the relay: " + fx.host.BuildReportJson();
				return false;
			}
			if (!fx.host.HasPendingRelayWork()) {
				*error = "a refused forward left the host owing nothing: " + fx.host.BuildReportJson();
				return false;
			}
			// Completing the round does not discharge the debt: the frames are still undelivered.
			fx.host.Complete("host reached its tick cap");
			if (!fx.host.HasPendingRelayWork()) {
				*error = "Complete() dropped the forwards the host still owed";
				return false;
			}
			fx.Collect(fx.clientB, bReady);
			const size_t behind = bReady.size();
			// The link comes back and the host is given the chance to hand over what it holds.
			fx.hostT.SetFaultConfig({});
			for (const uint64_t until = fx.now + 2000; fx.now <= until && fx.host.HasPendingRelayWork(); fx.now += 5) {
				fx.host.Tick(fx.now);
				fx.clientB.Tick(fx.now);
				fx.hostT.AdvanceTimeMs(5);
				fx.clientBT.AdvanceTimeMs(5);
			}
			if (fx.host.HasPendingRelayWork()) {
				*error = "the held forwards never drained: " + fx.host.BuildReportJson();
				return false;
			}
			for (const uint64_t until = fx.now + 500; fx.now <= until; fx.now += 5) {
				fx.clientB.Tick(fx.now);
				fx.clientBT.AdvanceTimeMs(5);
				fx.Collect(fx.clientB, bReady);
			}
			if (bReady.size() <= behind) {
				*error = "the peer that was owed forwards never received them";
				return false;
			}
			// The accounting the next lane run reads: bytes, not just packet counts.
			const NetLockstepStats& stats = fx.host.GetStats();
			if (stats.relayBytesSent == 0 || stats.largestRelayPacketBytes == 0 ||
			    stats.peers.at(3).relayBytesSent == 0 || stats.relayBacklogBytes != 0) {
				*error = "the relay byte accounting is missing: " + fx.host.BuildReportJson();
				return false;
			}
			const std::string report = fx.host.BuildReportJson();
			if (report.find("\"relay_bytes_sent\":") == std::string::npos ||
			    report.find("\"largest_relay_packet_bytes\":") == std::string::npos ||
			    report.find("\"relay_backlog_bytes\":") == std::string::npos) {
				*error = "the relay byte counters are missing from the report: " + report;
				return false;
			}
			return true;
		}
		// These fixtures drive a handshake that a build without the fixes answers with another start, so
		// they stop on a start budget as well as a clock: a fixture must fail, never fill the machine.
		constexpr uint32_t c_RoundStartBudget = 200;

		uint32_t StartsSent(std::initializer_list<const NetLockstepCoordinator*> peers) {
			uint32_t sent = 0;
			for (const NetLockstepCoordinator* peer: peers) {
				sent += peer->GetStats().startPacketsSent;
			}
			return sent;
		}

		// Every start a running peer receives reads as a repeat, so an answer that is itself a start
		// used to answer the answer: one stray start bounced between two peers forever.
		bool TestCoordinatorRepeatedStartsDoNotAmplify(std::string* error) {
			const uint16_t port = 43102;
			const uint64_t sessionId = 0x70000000000000A2ULL;
			const uint64_t roundId = 0x9A5E0000000000A2ULL;
			LoopbackTransport hostTransport, clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = roundId;
			hostConfig.timeoutMs = 20000;
			clientConfig.timeoutMs = 20000;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			uint64_t now = 0;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 5, now += 5) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					if (StartsSent({&host, &client}) > c_RoundStartBudget) {
						return false;
					}
					hostTransport.AdvanceTimeMs(5);
					clientTransport.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); }, 1000)) {
				*error = "the round never started";
				return false;
			}
			const uint32_t hostSentBefore = host.GetStats().startPacketsSent;
			const uint32_t clientSentBefore = client.GetStats().startPacketsSent;
			NetLockstepStart repeat;
			repeat.sessionId = sessionId;
			repeat.startFrame = 0;
			repeat.inputDelayFrames = 0;
			repeat.controllerFrameVersion = ControllerFrame::c_Version;
			repeat.controllerFrameEncodedSize = static_cast<uint16_t>(ControllerFrame::c_EncodedSize);
			repeat.localPeerId = 2;
			repeat.peerCount = 2;
			repeat.scenario = "LockstepSelfTest";
			repeat.ownershipPolicy = "unique-id-split";
			repeat.roundId = roundId;
			std::vector<uint8_t> bytes;
			if (!EncodePacket({repeat}, bytes, error) || !clientTransport.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			drive([&] { return false; }, 1000);
			const uint32_t hostSent = host.GetStats().startPacketsSent - hostSentBefore;
			const uint32_t clientSent = client.GetStats().startPacketsSent - clientSentBefore;
			// One second is four retransmit intervals; a start each way per interval is the whole budget.
			if (hostSent > 8 || clientSent > 8) {
				*error = "one repeated start amplified into " + std::to_string(hostSent) + " host and " +
				         std::to_string(clientSent) + " client starts in a second";
				return false;
			}
			if (host.IsFailed() || client.IsFailed() || !host.IsRunning() || !client.IsRunning()) {
				*error = "a repeated start disturbed the running round";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS repeated_starts_do_not_amplify host=" << hostSent << " client=" << clientSent << std::endl;
			return true;
		}
		// A peer repeating its start is missing one and cannot say whose, so the host owes it the whole
		// round: its own start and every remote start it has taken. Client B starts late here, so the
		// relayed start of client A drains into the void and only a re-relay can reach it; client C
		// starts later still, so the host is answering while it is itself waiting for a start.
		bool TestCoordinatorRepeatedStartCarriesTheRound(std::string* error) {
			const uint16_t port = 43100;
			const uint64_t sessionId = 0x70000000000000A0ULL;
			const uint64_t roundId = 0x9A5E0000000000A0ULL;
			LoopbackTransport hostT, clientAT, clientBT, clientCT;
			if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) ||
			    !clientBT.Connect("loopback", port, error) || !clientCT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.startFrame = 0;
				c.inputDelayFrames = 0;
				c.timeoutMs = 20000;
				c.localPeerId = local;
				c.peerCount = 4;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.frameLane = NetTransportLane::ControlReliable;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				c.roundId = relay ? roundId : 0;
				return c;
			};
			NetLockstepCoordinator host, clientA, clientB, clientC;
			uint64_t now = 0;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 5, now += 5) {
					host.Tick(now);
					clientA.Tick(now);
					clientB.Tick(now);
					clientC.Tick(now);
					if (done()) {
						return true;
					}
					if (StartsSent({&host, &clientA, &clientB, &clientC}) > c_RoundStartBudget) {
						return false;
					}
					hostT.AdvanceTimeMs(5);
					clientAT.AdvanceTimeMs(5);
					clientBT.AdvanceTimeMs(5);
					clientCT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}, {4, 3}}, true), error) || !clientA.Start(clientAT, cfg(2, {{1, 1}}, false), error)) {
				return false;
			}
			if (!drive([&] { return host.GetStats().startPacketsReceived >= 1; }, 500)) {
				*error = "the host never took client A's start";
				return false;
			}
			// Client B is between rounds: the host's start and the relayed start of A drain into the void.
			clientBT.AdvanceTimeMs(10);
			if (clientBT.PollEvents().empty()) {
				*error = "the relayed start never reached client B's transport";
				return false;
			}
			if (!clientB.Start(clientBT, cfg(3, {{1, 1}}, false), error)) {
				return false;
			}
			// B repeats while the host still waits for C, so the answer has to come from a waiting host.
			if (!drive([&] { return clientB.GetStats().startRetransmits >= 2; }, 1500)) {
				*error = "client B never repeated its start";
				return false;
			}
			if (host.IsRunning()) {
				*error = "the host ran before client C started, so this fixture proves nothing";
				return false;
			}
			if (!clientC.Start(clientCT, cfg(4, {{1, 1}}, false), error)) {
				return false;
			}
			if (!drive([&] { return host.IsRunning() && clientA.IsRunning() && clientB.IsRunning() && clientC.IsRunning(); }, 3000)) {
				*error = "a repeated start was not answered with the round: " + clientB.BuildReportJson();
				return false;
			}
			if (host.GetStats().startsRelayedOnRepeat == 0) {
				*error = "client B reached Running without the host re-relaying a start";
				return false;
			}
			for (uint64_t producedFrame = 0; producedFrame < 3; ++producedFrame) {
				if (!host.QueueLocalInput(producedFrame, {MakeFrame(100 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, {}, error) ||
				    !clientA.QueueLocalInput(producedFrame, {MakeFrame(200 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, {}, error) ||
				    !clientB.QueueLocalInput(producedFrame, {MakeFrame(300 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, {}, error) ||
				    !clientC.QueueLocalInput(producedFrame, {MakeFrame(400 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, {}, error)) {
					return false;
				}
			}
			std::vector<uint64_t> hostReady, aReady, bReady, cReady;
			if (!drive([&] {
					DrainReady(host, hostReady);
					DrainReady(clientA, aReady);
					DrainReady(clientB, bReady);
					DrainReady(clientC, cReady);
					return hostReady.size() == 3 && aReady.size() == 3 && bReady.size() == 3 && cReady.size() == 3;
				}, 2000)) {
				*error = "the re-formed four-peer round did not commit";
				return false;
			}
			// Another remote's start is not this peer's round to move: relayed, so it never owns the
			// transport it arrives on, and a round tag it disagrees with makes it a straggler.
			const uint64_t staleBefore = clientB.GetStats().staleRoundPackets;
			NetLockstepStart relayedStraggler;
			relayedStraggler.sessionId = sessionId;
			relayedStraggler.startFrame = 0;
			relayedStraggler.inputDelayFrames = 0;
			relayedStraggler.controllerFrameVersion = ControllerFrame::c_Version;
			relayedStraggler.controllerFrameEncodedSize = static_cast<uint16_t>(ControllerFrame::c_EncodedSize);
			relayedStraggler.localPeerId = 2;
			relayedStraggler.peerCount = 4;
			relayedStraggler.scenario = "LockstepSelfTest";
			relayedStraggler.ownershipPolicy = "unique-id-split";
			relayedStraggler.roundId = roundId + 1;
			std::vector<uint8_t> bytes;
			if (!EncodePacket({relayedStraggler}, bytes, error) || !hostT.Send(2, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			if (!drive([&] { return clientB.GetStats().staleRoundPackets == staleBefore + 1; }, 500)) {
				*error = "a relayed start from another round was not ignored";
				return false;
			}
			if (clientB.GetRoundId() != roundId || clientB.IsFailed()) {
				*error = "a relayed start moved client B's round";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS repeated_start_carries_the_round relayed=" << host.GetStats().startsRelayedOnRepeat << std::endl;
			return true;
		}
	}

	int NetLockstepSelfTest::Run() {
		auto fail = [](const std::string& message) {
			std::cerr << "[net-lockstep-selftest] FAIL: " << message << std::endl;
			return 1;
		};

		std::string error;
		if (!TestRoundTrips(&error) ||
		    !TestCoordinatorRepeatedStartCarriesTheRound(&error) ||
		    !TestCoordinatorRepeatedStartsDoNotAmplify(&error) ||
		    !TestObservationSlotCodec(&error) ||
		    !TestCanonicalHeader(&error) ||
		    !TestDecodeFailures(&error) ||
		    !TestSemanticFailures(&error) ||
		    !TestRecoveryStopsAtCompletedTick(&error) ||
		    !TestCompletionDrainsAppliedTicks(&error) ||
		    !TestCoordinatorDelayedHappyPath(&error) ||
		    !TestCoordinatorFrameBeforeStart(&error) ||
		    !TestCoordinatorIgnoresStaleRound(&error) ||
		    !TestCoordinatorPerSenderDelay(&error) ||
		    !TestCoordinatorPerSenderDelayMismatch(&error) ||
		    !TestCoordinatorIgnoresSessionPacketsAtHandoff(&error) ||
		    !TestCoordinatorUnreliableOutOfOrderDuplicate(&error) ||
		    !TestCoordinatorMissingFrameTimeout(&error) ||
		    !TestCoordinatorThreePeer(&error) ||
		    !TestCoordinatorPeerLeave(&error) ||
		    !TestCoordinatorHostAdjudicatesSilentPeer(&error) ||
		    !TestCoordinatorSilentHostStillTimesOut(&error) ||
		    !TestCoordinatorRelayBacklogHeals(&error) ||
		    !TestCoordinatorRelayFailureDropsPeer(&error) ||
		    !TestCoordinatorDroppedSeatHold(&error) ||
		    !TestCoordinatorHeldSeatKeepsPlaying(&error) ||
		    !TestSeatStateNeverReadUnderTheServiceLock(&error) ||
		    !TestSessionPumpRunsWhileTheRoundWaits(&error) ||
		    !TestCoordinatorAdjudicatedPeerKeepsItsSeat(&error) ||
		    !TestRelayHostFinishesWhatItOwes(&error) ||
		    !TestObservationOverflowCarry(&error) ||
		    !TestStaleRoundFrameStillCountsAsTraffic(&error) ||
		    !TestObservationFaultsAreToldApart(&error) ||
		    !TestStaleRoundFrameLeavesTheLiveTable(&error) ||
		    !TestResyncStragglersAreNotHoles(&error) ||
		    !TestRefusedBlockLeavesNoBindings(&error) ||
		    !TestFourPeerObservationRelayBytes(&error)) {
			return fail(error);
		}

		std::cout << "[net-lockstep-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
