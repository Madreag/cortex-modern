#include "NetProtocolSelfTest.h"

#include "LoopbackTransport.h"
#include "NetProtocol.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace RTE {

	namespace {
		NetHash32 MakeHash(uint8_t seed) {
			NetHash32 hash{};
			for (size_t i = 0; i < hash.size(); ++i) {
				hash[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i));
			}
			return hash;
		}

		NetClientHello MakeClientHello() {
			NetClientHello payload;
			payload.clientNonce = 0x1122334455667788ULL;
			payload.minProtocolVersion = 1;
			payload.maxProtocolVersion = NetProtocol::c_Version;
			payload.controllerFrameVersion = 5;
			payload.controllerFrameEncodedSize = 80;
			payload.platformId = 1;
			payload.displayName = "Player";
			payload.gameVersion = "7.0.0";
			payload.buildId = "stage2-p2a";
			payload.deterministicConfigHash = MakeHash(1);
			payload.moduleManifestHash = MakeHash(33);
			payload.sessionRulesHash = MakeHash(65);
			payload.sessionIdentityHash = MakeHash(97);
			payload.hasUserdataModules = false;
			return payload;
		}

		template <size_t N>
		std::array<uint8_t, N> MakeBytes(uint8_t seed) {
			std::array<uint8_t, N> bytes{};
			for (size_t i = 0; i < bytes.size(); ++i) {
				bytes[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i));
			}
			return bytes;
		}

		NetH4Identity MakeH4Identity() {
			NetH4Identity identity;
			identity.controllerFrameVersion = 5;
			identity.controllerFrameEncodedSize = 80;
			identity.gameVersion = "7.0.0";
			identity.buildId = "stage2-h4a";
			identity.deterministicConfigHash = MakeHash(1);
			identity.moduleManifestHash = MakeHash(33);
			identity.sessionRulesHash = MakeHash(65);
			identity.sessionIdentityHash = MakeHash(97);
			return identity;
		}

		bool ExpectDecodeError(const std::vector<uint8_t>& bytes, NetProtocolErrorCode code, std::string* error) {
			const NetDecodeResult result = NetProtocol::Decode(bytes);
			if (result.ok) {
				*error = "expected decode failure for " + std::string(NetProtocol::ErrorCodeName(code));
				return false;
			}
			if (result.error.code != code) {
				*error = "expected " + std::string(NetProtocol::ErrorCodeName(code)) +
				         " got " + NetProtocol::ErrorCodeName(result.error.code) +
				         ": " + result.error.message;
				return false;
			}
			return true;
		}

		bool EncodeMessage(const NetMessage& message, std::vector<uint8_t>& bytes, std::string* error) {
			NetProtocolError encodeError;
			if (!NetProtocol::Encode(message, bytes, &encodeError)) {
				*error = "encode failed: " + encodeError.message;
				return false;
			}
			return true;
		}

		bool RoundTrip(const NetMessage& message, std::string* error) {
			std::vector<uint8_t> bytes;
			if (!EncodeMessage(message, bytes, error)) {
				return false;
			}
			const NetDecodeResult decoded = NetProtocol::Decode(bytes);
			if (!decoded.ok) {
				*error = "decode failed: " + decoded.error.message;
				return false;
			}
			if (!(decoded.message == message)) {
				*error = "decoded message differed for " + std::string(NetProtocol::MessageTypeName(NetProtocol::MessageTypeOf(message.payload)));
				return false;
			}
			return true;
		}

		bool TestRoundTrips(std::string* error) {
			const NetClientHello clientHello = MakeClientHello();
			NetHostHello hostHello;
			hostHello.sessionId = 0xAABBCCDDEEFF0011ULL;
			hostHello.hostNonce = 0x0102030405060708ULL;
			hostHello.selectedProtocolVersion = NetProtocol::c_Version;
			hostHello.controllerFrameVersion = 5;
			hostHello.controllerFrameEncodedSize = 80;
			hostHello.assignedPeerId = 1;
			hostHello.maxPeers = 4;
			hostHello.hostPlatformId = 2;
			hostHello.gameVersion = "7.0.0";
			hostHello.hostName = "Host";
			hostHello.buildId = "stage2-p2a";
			hostHello.deterministicConfigHash = MakeHash(1);
			hostHello.moduleManifestHash = MakeHash(33);
			hostHello.sessionRulesHash = MakeHash(65);
			hostHello.sessionIdentityHash = MakeHash(97);
			hostHello.hasUserdataModules = false;

			const std::vector<NetMessage> messages = {
				{1, 0, clientHello},
				{2, 0, hostHello},
				{3, 0, NetJoinAccepted{0xAABBCCDDEEFF0011ULL, 1, 4, NetProtocol::c_Version, 1000, 5000}},
				{4, 0, NetJoinRejected{NetRejectReason::ModuleManifestMismatch, "Module mismatch", "module_manifest_hash", "aaa", "bbb"}},
				{5, 0, NetReadyState{1, true, MakeHash(1), MakeHash(33)}},
				{6, 0, NetHeartbeat{1234, 5, 2}},
				{7, 0, NetPing{42, 1234}},
				{8, 0, NetPong{42, 1240}},
				{9, 0, NetDisconnect{2, "bye"}},
				{10, 0, NetSessionSummary{0xAABBCCDDEEFF0011ULL, 2, 1, 3, MakeHash(1), MakeHash(33)}},
			};

			for (const NetMessage& message : messages) {
				if (!RoundTrip(message, error)) {
					return false;
				}
			}

			NetClientHello maxLengthName = clientHello;
			maxLengthName.displayName.assign(NetProtocol::c_MaxDisplayNameBytes, 'x');
			if (!RoundTrip({11, 0, maxLengthName}, error)) {
				return false;
			}

			const NetJoinRejected emptyDiagnostics{NetRejectReason::HostNotAccepting, "", "", "", ""};
			if (!RoundTrip({12, 0, emptyDiagnostics}, error)) {
				return false;
			}
			for (uint16_t raw = 1; raw <= static_cast<uint16_t>(NetRejectReason::SeatReassigned); ++raw) {
				const NetRejectReason reason = static_cast<NetRejectReason>(raw);
				if (std::string(NetProtocol::RejectReasonName(reason)) == "Unknown") {
					*error = "reject reason " + std::to_string(raw) + " has no name";
					return false;
				}
				if (!RoundTrip({13, 0, NetJoinRejected{reason, "refused", "key", "", ""}}, error)) {
					*error = "reject reason " + std::to_string(raw) + " did not survive the wire: " + *error;
					return false;
				}
			}
			std::vector<uint8_t> beyond;
			if (!EncodeMessage({14, 0, NetJoinRejected{static_cast<NetRejectReason>(static_cast<uint16_t>(NetRejectReason::SeatReassigned) + 1U), "refused", "key", "", ""}}, beyond, error)) {
				return false;
			}
			if (!ExpectDecodeError(beyond, NetProtocolErrorCode::InvalidValue, error)) {
				return false;
			}
			return true;
		}

		bool TestCanonicalHeader(std::string* error) {
			std::vector<uint8_t> bytes;
			if (!EncodeMessage({0x01020304U, 0, NetPing{0x1122334455667788ULL, 0x0102030405060708ULL}}, bytes, error)) {
				return false;
			}
			if (bytes.size() != NetProtocol::c_HeaderBytes + 16U) {
				*error = "canonical ping encoded size mismatch";
				return false;
			}
			const std::vector<uint8_t> expectedPrefix = {
				0x43, 0x43, 0x4E, 0x32,
				0x02, 0x00,
				0x18, 0x00,
				0x07, 0x00,
				0x00, 0x00,
				0x04, 0x03, 0x02, 0x01,
				0x10, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
			};
			for (size_t i = 0; i < expectedPrefix.size(); ++i) {
				if (bytes[i] != expectedPrefix[i]) {
					*error = "canonical header byte mismatch at " + std::to_string(i);
					return false;
				}
			}
			if (bytes[24] != 0x88U || bytes[25] != 0x77U || bytes[31] != 0x11U) {
				*error = "canonical payload is not little-endian";
				return false;
			}
			return true;
		}

		bool TestH4RoundTrips(std::string* error) {
			NetH4NewJoin newJoin;
			newJoin.txId = MakeBytes<16>(0x10);
			newJoin.identity = MakeH4Identity();
			newJoin.displayName = "Player";

			NetH4Reclaim reclaim;
			reclaim.txId = MakeBytes<16>(0x20);
			reclaim.epoch = MakeBytes<16>(0x30);
			reclaim.stableSeat = 2;
			reclaim.holderGeneration = 7;
			reclaim.identity = MakeH4Identity();
			reclaim.displayName = "Player";

			// Every fixed-width admission message, with the size the wire schema fixes for it.
			const std::vector<std::pair<NetMessage, size_t>> messages = {
				{{21, 0, NetH4TicketOffer{c_NetH4Version, MakeBytes<16>(0x40), MakeBytes<16>(0x50), 3, 1, MakeBytes<32>(0x60), 0xAABBCCDDEEFF0011ULL, 20000}}, 84},
				{{22, 0, NetH4TicketStoredAck{c_NetH4Version, MakeBytes<16>(0x40), 3, 1, true}}, 28},
				{{23, 0, NetH4JoinCommitted{c_NetH4Version, MakeBytes<16>(0x40), 3, 1, 1, 2}}, 32},
				{{25, 0, NetH4Challenge{c_NetH4Version, MakeBytes<16>(0x20), MakeBytes<32>(0x70), 10000}}, 54},
				{{26, 0, NetH4Proof{c_NetH4Version, MakeBytes<16>(0x20), MakeBytes<16>(0x30), 2, 7, MakeBytes<16>(0x80), MakeBytes<32>(0x90)}}, 88},
				{{27, 0, NetH4LeaveRequest{c_NetH4Version, MakeBytes<16>(0xA0), MakeBytes<16>(0x30), 2, 7}}, 40},
				{{28, 0, NetH4LeaveAck{c_NetH4Version, MakeBytes<16>(0xA0), 2, 7, true}}, 28},
				{{30, 0, NetH4ApplicantAck{c_NetH4Version, MakeBytes<16>(0xB0), 1, 20000}}, 24},
				{{31, 0, NetH4SubstitutionOffer{c_NetH4Version, MakeBytes<16>(0xB0), MakeBytes<16>(0x50), 1, 2, MakeBytes<32>(0x60), MakeBytes<32>(0x70), 0xAABBCCDDEEFF0011ULL, 20000}}, 116},
				{{32, 0, NetH4SubstitutionAck{c_NetH4Version, MakeBytes<16>(0xB0), 1, 2, MakeBytes<16>(0x80), MakeBytes<32>(0x90), true}}, 76},
			};
			for (const auto& [message, payloadBytes] : messages) {
				if (!RoundTrip(message, error)) {
					return false;
				}
				std::vector<uint8_t> bytes;
				if (!EncodeMessage(message, bytes, error)) {
					return false;
				}
				if (bytes.size() - NetProtocol::c_HeaderBytes != payloadBytes) {
					*error = std::string(NetProtocol::MessageTypeName(NetProtocol::MessageTypeOf(message.payload))) +
					         " encoded " + std::to_string(bytes.size() - NetProtocol::c_HeaderBytes) + " payload bytes, not " + std::to_string(payloadBytes);
					return false;
				}
			}
			NetH4Applicant applicant;
			applicant.txId = MakeBytes<16>(0xB0);
			applicant.stableSeat = 1;
			applicant.identity = MakeH4Identity();
			applicant.displayName = "Substitute";
			if (!RoundTrip({20, 0, newJoin}, error) || !RoundTrip({24, 0, reclaim}, error) || !RoundTrip({33, 0, applicant}, error)) {
				return false;
			}

			// The Phase-B applicant is the second-widest admission message; the Reclaim stays the one
			// the cap is sized against.
			NetH4Applicant widestApplicant = applicant;
			widestApplicant.identity.gameVersion.assign(NetProtocol::c_MaxShortTextBytes, 'v');
			widestApplicant.identity.buildId.assign(NetProtocol::c_MaxShortTextBytes, 'b');
			widestApplicant.displayName.assign(NetProtocol::c_MaxDisplayNameBytes, 'n');
			std::vector<uint8_t> widestApplicantBytes;
			if (!EncodeMessage({34, 0, widestApplicant}, widestApplicantBytes, error)) {
				return false;
			}
			if (widestApplicantBytes.size() - NetProtocol::c_HeaderBytes != 478U) {
				*error = "worst-case Applicant payload is " + std::to_string(widestApplicantBytes.size() - NetProtocol::c_HeaderBytes) + " bytes, not 478";
				return false;
			}
			if (!RoundTrip({34, 0, widestApplicant}, error)) {
				return false;
			}

			// The worst-case admission message must fit the size the host refuses above, with headroom
			// for the Phase-B fields.
			NetH4Reclaim widest = reclaim;
			widest.identity.gameVersion.assign(NetProtocol::c_MaxShortTextBytes, 'v');
			widest.identity.buildId.assign(NetProtocol::c_MaxShortTextBytes, 'b');
			widest.displayName.assign(NetProtocol::c_MaxDisplayNameBytes, 'n');
			std::vector<uint8_t> widestBytes;
			if (!EncodeMessage({29, 0, widest}, widestBytes, error)) {
				return false;
			}
			// The cap is sized against this number, so pin it rather than only bounding it.
			if (widestBytes.size() - NetProtocol::c_HeaderBytes != 498U) {
				*error = "worst-case Reclaim payload is " + std::to_string(widestBytes.size() - NetProtocol::c_HeaderBytes) + " bytes, not 498";
				return false;
			}
			if (widestBytes.size() - NetProtocol::c_HeaderBytes > NetProtocol::c_MaxH4PayloadBytes) {
				*error = "worst-case Reclaim exceeds the admission size cap";
				return false;
			}
			if (!RoundTrip({29, 0, widest}, error)) {
				return false;
			}
			return true;
		}

		bool TestH4CanonicalBytes(std::string* error) {
			std::vector<uint8_t> bytes;
			if (!EncodeMessage({0x01020304U, 0, NetH4Challenge{c_NetH4Version, MakeBytes<16>(0x10), MakeBytes<32>(0x20), 10000}}, bytes, error)) {
				return false;
			}
			std::vector<uint8_t> expected = {
				0x43, 0x43, 0x4E, 0x32,
				0x02, 0x00,
				0x18, 0x00,
				0x10, 0x00,
				0x00, 0x00,
				0x04, 0x03, 0x02, 0x01,
				0x36, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x01, 0x00,
			};
			for (uint8_t i = 0; i < 16; ++i) {
				expected.push_back(static_cast<uint8_t>(0x10 + i));
			}
			for (uint8_t i = 0; i < 32; ++i) {
				expected.push_back(static_cast<uint8_t>(0x20 + i));
			}
			expected.insert(expected.end(), {0x10, 0x27, 0x00, 0x00});
			if (bytes != expected) {
				*error = "canonical Challenge bytes differed (size " + std::to_string(bytes.size()) + ")";
				return false;
			}

			if (!EncodeMessage({7, 0, NetH4LeaveAck{c_NetH4Version, MakeBytes<16>(0xA0), 2, 7, true}}, bytes, error)) {
				return false;
			}
			std::vector<uint8_t> expectedAck = {
				0x43, 0x43, 0x4E, 0x32,
				0x02, 0x00,
				0x18, 0x00,
				0x13, 0x00,
				0x00, 0x00,
				0x07, 0x00, 0x00, 0x00,
				0x1C, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x01, 0x00,
			};
			for (uint8_t i = 0; i < 16; ++i) {
				expectedAck.push_back(static_cast<uint8_t>(0xA0 + i));
			}
			expectedAck.insert(expectedAck.end(), {0x02, 0x00, 0x07, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00});
			if (bytes != expectedAck) {
				*error = "canonical LeaveAck bytes differed (size " + std::to_string(bytes.size()) + ")";
				return false;
			}

			if (!EncodeMessage({9, 0, NetH4SubstitutionAck{c_NetH4Version, MakeBytes<16>(0xB0), 1, 2, MakeBytes<16>(0x80), MakeBytes<32>(0x90), true}}, bytes, error)) {
				return false;
			}
			std::vector<uint8_t> expectedSubstitution = {
				0x43, 0x43, 0x4E, 0x32,
				0x02, 0x00,
				0x18, 0x00,
				0x17, 0x00,
				0x00, 0x00,
				0x09, 0x00, 0x00, 0x00,
				0x4C, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x01, 0x00,
			};
			for (uint8_t i = 0; i < 16; ++i) {
				expectedSubstitution.push_back(static_cast<uint8_t>(0xB0 + i));
			}
			expectedSubstitution.insert(expectedSubstitution.end(), {0x01, 0x00, 0x02, 0x00, 0x00, 0x00});
			for (uint8_t i = 0; i < 16; ++i) {
				expectedSubstitution.push_back(static_cast<uint8_t>(0x80 + i));
			}
			for (uint8_t i = 0; i < 32; ++i) {
				expectedSubstitution.push_back(static_cast<uint8_t>(0x90 + i));
			}
			expectedSubstitution.insert(expectedSubstitution.end(), {0x01, 0x00, 0x00, 0x00});
			if (bytes != expectedSubstitution) {
				*error = "canonical SubstitutionAck bytes differed (size " + std::to_string(bytes.size()) + ")";
				return false;
			}
			return true;
		}

		bool TestH4DecodeFailures(std::string* error) {
			std::vector<uint8_t> bytes;
			if (!EncodeMessage({1, 0, NetH4Challenge{c_NetH4Version, MakeBytes<16>(0x10), MakeBytes<32>(0x20), 10000}}, bytes, error)) {
				return false;
			}
			std::vector<uint8_t> mutated = bytes;
			mutated[NetProtocol::c_HeaderBytes] = 0x02U;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::UnsupportedVersion, error)) {
				return false;
			}

			// An oversized admission message is refused on the header, before any field is parsed.
			mutated.assign(NetProtocol::c_HeaderBytes + NetProtocol::c_MaxH4PayloadBytes + 1U, 0);
			mutated[0] = 0x43; mutated[1] = 0x43; mutated[2] = 0x4E; mutated[3] = 0x32;
			mutated[4] = 0x02;
			mutated[6] = 0x18;
			mutated[8] = static_cast<uint8_t>(NetMessageType::Reclaim);
			mutated[16] = static_cast<uint8_t>((NetProtocol::c_MaxH4PayloadBytes + 1U) & 0xFFU);
			mutated[17] = static_cast<uint8_t>(((NetProtocol::c_MaxH4PayloadBytes + 1U) >> 8) & 0xFFU);
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::PayloadTooLarge, error)) {
				return false;
			}

			NetH4Reclaim reclaim;
			reclaim.txId = MakeBytes<16>(0x20);
			reclaim.epoch = MakeBytes<16>(0x30);
			reclaim.stableSeat = 2;
			reclaim.holderGeneration = 7;
			reclaim.identity = MakeH4Identity();
			if (!EncodeMessage({2, 0, reclaim}, bytes, error)) {
				return false;
			}
			// Generation 0 names an unheld seat, so no message may claim it.
			mutated = bytes;
			for (size_t i = 0; i < 4; ++i) {
				mutated[NetProtocol::c_HeaderBytes + 36U + i] = 0;
			}
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::InvalidValue, error)) {
				return false;
			}
			mutated = bytes;
			mutated.resize(NetProtocol::c_HeaderBytes + 20U);
			mutated[16] = 20U;
			mutated[17] = 0;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::TruncatedPayload, error)) {
				return false;
			}

			if (!EncodeMessage({3, 0, NetH4LeaveAck{c_NetH4Version, MakeBytes<16>(0xA0), 2, 7, true}}, bytes, error)) {
				return false;
			}
			mutated = bytes;
			mutated[NetProtocol::c_HeaderBytes + 24U] = 2U;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::InvalidValue, error)) {
				return false;
			}
			mutated = bytes;
			mutated[NetProtocol::c_HeaderBytes + 25U] = 1U;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::ReservedFieldNonZero, error)) {
				return false;
			}

			if (!EncodeMessage({4, 0, NetH4JoinCommitted{c_NetH4Version, MakeBytes<16>(0x40), 3, 1, 1, 2}}, bytes, error)) {
				return false;
			}
			mutated = bytes;
			for (size_t i = 0; i < 4; ++i) {
				mutated[NetProtocol::c_HeaderBytes + 24U + i] = 0;
			}
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::InvalidValue, error)) {
				return false;
			}

			if (!EncodeMessage({6, 0, NetH4SubstitutionAck{c_NetH4Version, MakeBytes<16>(0xB0), 1, 2, MakeBytes<16>(0x80), MakeBytes<32>(0x90), true}}, bytes, error)) {
				return false;
			}
			mutated = bytes;
			for (size_t i = 0; i < 4; ++i) {
				mutated[NetProtocol::c_HeaderBytes + 20U + i] = 0;
			}
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::InvalidValue, error)) {
				return false;
			}
			mutated = bytes;
			mutated[NetProtocol::c_HeaderBytes + 72U] = 2U;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::InvalidValue, error)) {
				return false;
			}
			mutated = bytes;
			mutated[NetProtocol::c_HeaderBytes + 73U] = 1U;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::ReservedFieldNonZero, error)) {
				return false;
			}

			if (!EncodeMessage({7, 0, NetH4SubstitutionOffer{c_NetH4Version, MakeBytes<16>(0xB0), MakeBytes<16>(0x50), 1, 2, MakeBytes<32>(0x60), MakeBytes<32>(0x70), 1, 20000}}, bytes, error)) {
				return false;
			}
			mutated = bytes;
			for (size_t i = 0; i < 4; ++i) {
				mutated[NetProtocol::c_HeaderBytes + 36U + i] = 0;
			}
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::InvalidValue, error)) {
				return false;
			}

			NetProtocolError encodeError;
			std::vector<uint8_t> ignored;
			NetH4NewJoin longName;
			longName.txId = MakeBytes<16>(0x10);
			longName.identity = MakeH4Identity();
			longName.displayName.assign(NetProtocol::c_MaxDisplayNameBytes + 1U, 'a');
			if (NetProtocol::Encode({5, 0, longName}, ignored, &encodeError) || encodeError.code != NetProtocolErrorCode::StringTooLong) {
				*error = "overlong H4 display name was accepted";
				return false;
			}
			return true;
		}

		bool TestDecodeFailures(std::string* error) {
			std::vector<uint8_t> bytes;
			if (!EncodeMessage({1, 0, NetPing{1, 2}}, bytes, error)) {
				return false;
			}

			if (!ExpectDecodeError({}, NetProtocolErrorCode::ShortHeader, error)) {
				return false;
			}

			std::vector<uint8_t> mutated = bytes;
			mutated[0] = 0;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::BadMagic, error)) {
				return false;
			}

			mutated = bytes;
			mutated[4] = 0xFFU;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::UnsupportedVersion, error)) {
				return false;
			}

			mutated = bytes;
			mutated[6] = 0x10U;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::BadHeaderSize, error)) {
				return false;
			}

			mutated = bytes;
			mutated[8] = 0xFEU;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::UnknownMessageType, error)) {
				return false;
			}

			mutated = bytes;
			mutated[10] = 0x01U;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::UnknownFlags, error)) {
				return false;
			}

			mutated = bytes;
			mutated[20] = 0x01U;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::ReservedFieldNonZero, error)) {
				return false;
			}

			mutated = bytes;
			mutated[16] = 0xFFU;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::PayloadLengthMismatch, error)) {
				return false;
			}

			mutated = bytes;
			mutated[16] = 0x01U;
			mutated[18] = 0x01U;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::PayloadTooLarge, error)) {
				return false;
			}

			mutated = bytes;
			mutated.push_back(0);
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::PayloadLengthMismatch, error)) {
				return false;
			}

			mutated = bytes;
			mutated.resize(NetProtocol::c_HeaderBytes + 8U);
			mutated[16] = 0x08U;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::TruncatedPayload, error)) {
				return false;
			}

			if (!EncodeMessage({2, 0, NetReadyState{1, true, MakeHash(1), MakeHash(2)}}, bytes, error)) {
				return false;
			}
			mutated = bytes;
			mutated[NetProtocol::c_HeaderBytes + 1] = 2U;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::InvalidValue, error)) {
				return false;
			}

			mutated = bytes;
			mutated[NetProtocol::c_HeaderBytes + 2] = 1U;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::ReservedFieldNonZero, error)) {
				return false;
			}

			if (!EncodeMessage({6, 0, NetJoinRejected{NetRejectReason::Timeout, "", "", "", ""}}, bytes, error)) {
				return false;
			}
			mutated = bytes;
			mutated[NetProtocol::c_HeaderBytes] = 0xFEU;
			mutated[NetProtocol::c_HeaderBytes + 1] = 0xFFU;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::InvalidValue, error)) {
				return false;
			}

			NetProtocolError encodeError;
			std::vector<uint8_t> ignored;
			NetClientHello tooLongName = MakeClientHello();
			tooLongName.displayName.assign(NetProtocol::c_MaxDisplayNameBytes + 1U, 'a');
			if (NetProtocol::Encode({3, 0, tooLongName}, ignored, &encodeError) || encodeError.code != NetProtocolErrorCode::StringTooLong) {
				*error = "overlong display name was accepted";
				return false;
			}
			NetClientHello controlName = MakeClientHello();
			controlName.displayName = "bad\nname";
			if (NetProtocol::Encode({4, 0, controlName}, ignored, &encodeError) || encodeError.code != NetProtocolErrorCode::InvalidString) {
				*error = "control-char display name was accepted";
				return false;
			}
			if (NetProtocol::Encode({5, 1, NetPing{1, 2}}, ignored, &encodeError) || encodeError.code != NetProtocolErrorCode::UnknownFlags) {
				*error = "nonzero encode flags were accepted";
				return false;
			}

			return true;
		}

		NetModuleDigestEntry MakeDigestEntry(const std::string& fileName, uint32_t version, uint8_t hashSeed, bool official = false) {
			NetModuleDigestEntry entry;
			entry.fileName = fileName;
			entry.friendlyName = fileName.substr(0, fileName.find('.'));
			entry.version = version;
			entry.official = official;
			entry.contentHash = MakeHash(hashSeed);
			return entry;
		}

		bool ExpectEncodeError(const NetMessage& message, NetProtocolErrorCode code, const std::string& what, std::string* error) {
			NetProtocolError encodeError;
			std::vector<uint8_t> ignored;
			if (NetProtocol::Encode(message, ignored, &encodeError)) {
				*error = what + " was accepted by the encoder";
				return false;
			}
			if (encodeError.code != code) {
				*error = what + " gave " + NetProtocol::ErrorCodeName(encodeError.code) + ", not " + NetProtocol::ErrorCodeName(code);
				return false;
			}
			return true;
		}

		bool TestModuleDigestRoundTrips(std::string* error) {
			NetModuleDigestRequest request;
			request.maxEntries = static_cast<uint16_t>(NetProtocol::c_MaxModuleDigestEntries);
			if (!RoundTrip({40, 0, request}, error)) {
				return false;
			}
			std::vector<uint8_t> requestBytes;
			if (!EncodeMessage({40, 0, request}, requestBytes, error)) {
				return false;
			}
			if (requestBytes.size() - NetProtocol::c_HeaderBytes != 4U) {
				*error = "ModuleDigestRequest encoded " + std::to_string(requestBytes.size() - NetProtocol::c_HeaderBytes) + " payload bytes, not 4";
				return false;
			}

			NetModuleDigests digests;
			digests.entries = {MakeDigestEntry("Base.rte", 1, 129, true), MakeDigestEntry("Coalition.rte", 3, 161), MakeDigestEntry("Ronin.rte", 5, 193)};
			if (!RoundTrip({41, 0, digests}, error)) {
				return false;
			}
			NetModuleDigests truncated = digests;
			truncated.flags = c_NetModuleDigestsTruncated;
			if (!RoundTrip({42, 0, truncated}, error)) {
				return false;
			}
			NetModuleDigests empty;
			if (!RoundTrip({43, 0, empty}, error)) {
				return false;
			}

			// Sorted and unique by file name both ways: the encoder refuses to write an unsorted list
			// and the decoder refuses to read one.
			NetModuleDigests unsorted;
			unsorted.entries = {MakeDigestEntry("Ronin.rte", 5, 193), MakeDigestEntry("Base.rte", 1, 129)};
			if (!ExpectEncodeError({44, 0, unsorted}, NetProtocolErrorCode::InvalidValue, "an unsorted digest list", error)) {
				return false;
			}
			NetModuleDigests duplicate;
			duplicate.entries = {MakeDigestEntry("Base.rte", 1, 129), MakeDigestEntry("Base.rte", 2, 130)};
			if (!ExpectEncodeError({45, 0, duplicate}, NetProtocolErrorCode::InvalidValue, "a duplicated digest entry", error)) {
				return false;
			}
			std::vector<uint8_t> sortedBytes;
			if (!EncodeMessage({46, 0, digests}, sortedBytes, error)) {
				return false;
			}
			// Swap the two leading entries' names on the wire; the decoder must refuse the result.
			std::vector<uint8_t> mutated = sortedBytes;
			const size_t firstName = NetProtocol::c_HeaderBytes + 6U + 2U;
			mutated[firstName] = 'Z';
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::InvalidValue, error)) {
				return false;
			}
			return true;
		}

		bool TestModuleDigestCaps(std::string* error) {
			NetModuleDigests overCount;
			for (size_t i = 0; i <= NetProtocol::c_MaxModuleDigestEntries; ++i) {
				std::string name = std::to_string(100000 + i) + ".rte";
				overCount.entries.push_back(MakeDigestEntry(name, 1, static_cast<uint8_t>(i)));
			}
			if (!ExpectEncodeError({47, 0, overCount}, NetProtocolErrorCode::InvalidValue, "a digest list over the entry cap", error)) {
				return false;
			}

			// The entry cap alone does not bound the bytes: 256 worst-case names do not fit, and the
			// encoder must refuse rather than write a packet the decoder would drop on the header.
			NetModuleDigests overBytes;
			for (size_t i = 0; i < NetProtocol::c_MaxModuleDigestEntries; ++i) {
				NetModuleDigestEntry entry;
				entry.fileName = std::to_string(100000 + i) + std::string(NetProtocol::c_MaxModuleNameBytes - 6U, 'n');
				entry.friendlyName.assign(NetProtocol::c_MaxModuleNameBytes, 'f');
				entry.version = 1;
				entry.contentHash = MakeHash(static_cast<uint8_t>(i));
				overBytes.entries.push_back(std::move(entry));
			}
			if (!ExpectEncodeError({48, 0, overBytes}, NetProtocolErrorCode::PayloadTooLarge, "a digest list over the byte cap", error)) {
				return false;
			}

			NetModuleDigests atCap;
			for (size_t i = 0; i < NetProtocol::c_MaxModuleDigestEntries; ++i) {
				atCap.entries.push_back(MakeDigestEntry(std::to_string(100000 + i) + ".rte", static_cast<uint32_t>(i), static_cast<uint8_t>(i)));
			}
			std::vector<uint8_t> atCapBytes;
			if (!EncodeMessage({49, 0, atCap}, atCapBytes, error)) {
				return false;
			}
			if (atCapBytes.size() - NetProtocol::c_HeaderBytes > NetProtocol::c_MaxModuleDigestBytes) {
				*error = "a full-cap digest list of plain names did not fit the byte cap";
				return false;
			}
			if (!RoundTrip({49, 0, atCap}, error)) {
				return false;
			}

			// Refused on the header, before any entry is parsed.
			std::vector<uint8_t> oversized(NetProtocol::c_HeaderBytes + NetProtocol::c_MaxModuleDigestBytes + 1U, 0);
			oversized[0] = 0x43; oversized[1] = 0x43; oversized[2] = 0x4E; oversized[3] = 0x32;
			oversized[4] = static_cast<uint8_t>(NetProtocol::c_Version);
			oversized[6] = 0x18;
			oversized[8] = static_cast<uint8_t>(NetMessageType::ModuleDigests);
			const uint32_t payloadSize = static_cast<uint32_t>(NetProtocol::c_MaxModuleDigestBytes + 1U);
			for (int i = 0; i < 4; ++i) {
				oversized[16 + i] = static_cast<uint8_t>((payloadSize >> (i * 8)) & 0xFFU);
			}
			if (!ExpectDecodeError(oversized, NetProtocolErrorCode::PayloadTooLarge, error)) {
				return false;
			}

			NetModuleDigestRequest zero;
			zero.maxEntries = 0;
			if (!ExpectEncodeError({50, 0, zero}, NetProtocolErrorCode::InvalidValue, "a digest request for zero entries", error)) {
				return false;
			}
			NetModuleDigestRequest tooMany;
			tooMany.maxEntries = static_cast<uint16_t>(NetProtocol::c_MaxModuleDigestEntries + 1U);
			if (!ExpectEncodeError({51, 0, tooMany}, NetProtocolErrorCode::InvalidValue, "a digest request over the entry cap", error)) {
				return false;
			}
			std::cout << "[net-protocol-selftest] PASS module digests: entry cap " << NetProtocol::c_MaxModuleDigestEntries
			          << ", byte cap " << NetProtocol::c_MaxModuleDigestBytes << ", full-cap list "
			          << (atCapBytes.size() - NetProtocol::c_HeaderBytes) << " bytes" << std::endl;
			return true;
		}

		bool TestChatRoundTrips(std::string* error) {
			NetChat chat;
			chat.senderPeerId = 2;
			chat.scope = c_NetChatScopeAll;
			chat.sentAtMs = 123456;
			chat.text = "gg, nice shot";
			if (!RoundTrip({52, 0, chat}, error)) {
				return false;
			}
			NetChat team = chat;
			team.scope = c_NetChatScopeTeam;
			team.text = "\xC3\xA9\xC3\xA0 flanking left \xE2\x86\x92";
			if (!RoundTrip({53, 0, team}, error)) {
				return false;
			}
			NetChat empty;
			if (!RoundTrip({54, 0, empty}, error)) {
				return false;
			}

			NetChat atCap = chat;
			atCap.text.assign(NetProtocol::c_MaxShortTextBytes, 'x');
			if (!RoundTrip({55, 0, atCap}, error)) {
				return false;
			}
			NetChat overCap = chat;
			overCap.text.assign(NetProtocol::c_MaxShortTextBytes + 1U, 'x');
			if (!ExpectEncodeError({56, 0, overCap}, NetProtocolErrorCode::StringTooLong, "a 129-byte chat line", error)) {
				return false;
			}
			// The decoder must answer the same bound: a wire that claims 129 bytes of chat text is
			// StringTooLong, not a disconnectable offence - a well-formed same-version peer's own
			// encoder could never have produced it, so the old build refuses identically.
			{
				std::vector<uint8_t> longChat;
				if (!EncodeMessage({62, 0, atCap}, longChat, error)) {
					return false;
				}
				const size_t textLenAt = NetProtocol::c_HeaderBytes + 8U; // version, sender, scope, sentAt
				longChat[textLenAt] = 129;
				longChat[textLenAt + 1U] = 0;
				++longChat[16]; // the header's payload length must grow with the claimed string
				longChat.push_back('x');
				if (!ExpectDecodeError(longChat, NetProtocolErrorCode::StringTooLong, error)) {
					return false;
				}
			}
			NetChat controlChars = chat;
			controlChars.text = "two\nlines";
			if (!ExpectEncodeError({57, 0, controlChars}, NetProtocolErrorCode::InvalidString, "a chat line with a newline", error)) {
				return false;
			}
			NetChat badScope = chat;
			badScope.scope = 2;
			if (!ExpectEncodeError({58, 0, badScope}, NetProtocolErrorCode::InvalidValue, "a chat line with an unknown scope", error)) {
				return false;
			}
			for (const std::string& bad : {std::string("\xC3"), std::string("\xC0\xAF"), std::string("\xED\xA0\x80"), std::string("\x80"), std::string("\xF5\x80\x80\x80")}) {
				NetChat invalid = chat;
				invalid.text = bad;
				if (!ExpectEncodeError({59, 0, invalid}, NetProtocolErrorCode::InvalidString, "a chat line that is not UTF-8", error)) {
					return false;
				}
			}
			std::vector<uint8_t> bytes;
			if (!EncodeMessage({60, 0, chat}, bytes, error)) {
				return false;
			}
			std::vector<uint8_t> mutated = bytes;
			mutated[NetProtocol::c_HeaderBytes + 3U] = 3U;
			if (!ExpectDecodeError(mutated, NetProtocolErrorCode::InvalidValue, error)) {
				return false;
			}
			// The malformed-chat path keys on the envelope type alone, so the peek must name Chat on
			// bytes the payload decoder rejects, and refuse everything that is not a session envelope.
			uint16_t peeked = 0;
			if (!NetProtocol::PeekMessageType(mutated.data(), mutated.size(), peeked) || peeked != static_cast<uint16_t>(NetMessageType::Chat)) {
				*error = "PeekMessageType did not name Chat on a chat envelope with a bad payload";
				return false;
			}
			if (NetProtocol::PeekMessageType(bytes.data(), NetProtocol::c_HeaderBytes - 1U, peeked) ||
			    NetProtocol::PeekMessageType(nullptr, 0, peeked)) {
				*error = "PeekMessageType accepted a truncated or absent envelope";
				return false;
			}
			NetHeartbeat heartbeat;
			std::vector<uint8_t> heartbeatBytes;
			if (!EncodeMessage({61, 0, heartbeat}, heartbeatBytes, error)) {
				return false;
			}
			if (!NetProtocol::PeekMessageType(heartbeatBytes.data(), heartbeatBytes.size(), peeked) ||
			    peeked != static_cast<uint16_t>(NetMessageType::Heartbeat)) {
				*error = "PeekMessageType misnamed a non-chat envelope";
				return false;
			}
			std::cout << "[net-protocol-selftest] PASS chat: " << NetProtocol::c_MaxShortTextBytes << "-byte cap, UTF-8 validated, scopes all/team" << std::endl;
			return true;
		}

		bool TestOldWireEncoding(std::string* error) {
			// A v1 peer's build has no decoder for the v2 types, so it is told why it was refused in
			// its own envelope and never handed a payload it would read as garbage.
			if (!NetProtocol::CanEncodeAtVersion(1) || !NetProtocol::CanEncodeAtVersion(NetProtocol::c_Version)) {
				*error = "the build cannot stamp a rejection at v1 and at its own version";
				return false;
			}
			if (NetProtocol::CanEncodeAtVersion(0) || NetProtocol::CanEncodeAtVersion(static_cast<uint16_t>(NetProtocol::c_Version + 1))) {
				*error = "a version this build cannot write was accepted";
				return false;
			}
			const NetMessage rejection{7, 0, NetJoinRejected{NetRejectReason::ProtocolMismatch, "protocol version 1 does not match this build's 2", "protocol_version", "2", "1"}};
			std::vector<uint8_t> v1;
			NetProtocolError encodeError;
			if (!NetProtocol::EncodeAtVersion(rejection, 1, v1, &encodeError)) {
				*error = "a rejection could not be stamped at v1: " + encodeError.message;
				return false;
			}
			uint16_t stamped = 0;
			if (!NetProtocol::PeekHeaderVersion(v1.data(), v1.size(), stamped) || stamped != 1U) {
				*error = "the v1 rejection was not stamped at v1";
				return false;
			}
			std::vector<uint8_t> current;
			if (!NetProtocol::EncodeAtVersion(rejection, NetProtocol::c_Version, current, &encodeError)) {
				*error = "a rejection could not be stamped at the current version: " + encodeError.message;
				return false;
			}
			// The payload schema did not change across the bump, so only the two version bytes differ.
			if (v1.size() != current.size()) {
				*error = "the v1 and v2 rejections differ in size";
				return false;
			}
			for (size_t i = 0; i < v1.size(); ++i) {
				if (i != 4 && i != 5 && v1[i] != current[i]) {
					*error = "the v1 rejection differs from the v2 one outside the version field, at byte " + std::to_string(i);
					return false;
				}
			}
			// Our own decoder only speaks the current version; the v1 bytes are for the peer that does.
			if (!ExpectDecodeError(v1, NetProtocolErrorCode::UnsupportedVersion, error)) {
				return false;
			}

			NetModuleDigestRequest request;
			request.maxEntries = 8;
			std::vector<uint8_t> refused;
			if (NetProtocol::EncodeAtVersion({8, 0, request}, 1, refused, &encodeError) ||
			    encodeError.code != NetProtocolErrorCode::UnsupportedVersion) {
				*error = "a v2-only message type was stamped at v1";
				return false;
			}
			NetChat chat;
			chat.text = "hello";
			if (NetProtocol::EncodeAtVersion({9, 0, chat}, 1, refused, &encodeError) ||
			    encodeError.code != NetProtocolErrorCode::UnsupportedVersion) {
				*error = "a v2-only chat message was stamped at v1";
				return false;
			}
			if (!NetProtocol::IsMessageTypeInVersion(NetMessageType::JoinRejected, 1) ||
			    NetProtocol::IsMessageTypeInVersion(NetMessageType::ModuleDigests, 1) ||
			    !NetProtocol::IsMessageTypeInVersion(NetMessageType::ModuleDigests, NetProtocol::c_Version)) {
				*error = "the per-version message type table is wrong";
				return false;
			}
			std::cout << "[net-protocol-selftest] PASS old wire: v" << NetProtocol::c_Version
			          << " build stamps a JoinRejected at v1, refuses v2-only types there" << std::endl;
			return true;
		}

		bool TestLoopback(std::string* error) {
			LoopbackTransport host;
			LoopbackTransport client;
			if (!host.StartHost(41001, error)) {
				return false;
			}
			if (!client.Connect("loopback", 41001, error)) {
				return false;
			}

			std::vector<NetTransportEvent> hostEvents = host.PollEvents();
			std::vector<NetTransportEvent> clientEvents = client.PollEvents();
			if (hostEvents.size() != 1U || hostEvents[0].type != NetTransportEventType::PeerConnected || hostEvents[0].peerId != 1U) {
				*error = "host did not receive expected PeerConnected";
				return false;
			}
			if (clientEvents.size() != 1U || clientEvents[0].type != NetTransportEventType::PeerConnected || clientEvents[0].peerId != 1U) {
				*error = "client did not receive expected PeerConnected";
				return false;
			}

			const std::vector<uint8_t> hello = {1, 2, 3};
			if (!client.Send(1, NetTransportLane::ControlReliable, hello, error)) {
				return false;
			}
			hostEvents = host.PollEvents();
			if (hostEvents.size() != 1U || hostEvents[0].type != NetTransportEventType::PacketReceived ||
			    hostEvents[0].peerId != 1U || hostEvents[0].bytes != hello) {
				*error = "host did not receive client packet";
				return false;
			}

			const std::vector<uint8_t> accepted = {4, 5};
			if (!host.Send(1, NetTransportLane::ControlReliable, accepted, error)) {
				return false;
			}
			clientEvents = client.PollEvents();
			if (clientEvents.size() != 1U || clientEvents[0].type != NetTransportEventType::PacketReceived ||
			    clientEvents[0].peerId != 1U || clientEvents[0].bytes != accepted) {
				*error = "client did not receive host packet";
				return false;
			}

			host.Disconnect(1, "done");
			hostEvents = host.PollEvents();
			clientEvents = client.PollEvents();
			if (hostEvents.size() != 1U || hostEvents[0].type != NetTransportEventType::PeerDisconnected ||
			    clientEvents.size() != 1U || clientEvents[0].type != NetTransportEventType::PeerDisconnected) {
				*error = "disconnect events were not symmetric";
				return false;
			}

			return true;
		}

		bool TestLoopbackFaults(std::string* error) {
			LoopbackTransport host;
			LoopbackTransport client;
			LoopbackTransportConfig config;
			config.latencyMs = 10;
			config.reorderUnreliable = true;
			config.unreliableDropEveryN = 2;
			client.SetFaultConfig(config);

			if (!host.StartHost(41002, error) || !client.Connect("loopback", 41002, error)) {
				return false;
			}
			host.PollEvents();
			client.PollEvents();

			if (!client.Send(1, NetTransportLane::ControlReliable, {1}, error) ||
			    !client.Send(1, NetTransportLane::ControlReliable, {2}, error)) {
				return false;
			}
			if (!host.PollEvents().empty()) {
				*error = "latency-delayed reliable packets arrived early";
				return false;
			}
			host.AdvanceTimeMs(10);
			std::vector<NetTransportEvent> events = host.PollEvents();
			if (events.size() != 2U || events[0].bytes != std::vector<uint8_t>{1} || events[1].bytes != std::vector<uint8_t>{2}) {
				*error = "reliable control order was not preserved";
				return false;
			}

			if (!client.Send(1, NetTransportLane::InputUnreliable, {3}, error) ||
			    !client.Send(1, NetTransportLane::InputUnreliable, {4}, error) ||
			    !client.Send(1, NetTransportLane::InputUnreliable, {5}, error)) {
				return false;
			}
			host.AdvanceTimeMs(20);
			events = host.PollEvents();
			if (events.size() != 2U || events[0].bytes != std::vector<uint8_t>{3} || events[1].bytes != std::vector<uint8_t>{5}) {
				*error = "unreliable deterministic drop behavior differed";
				return false;
			}

			return true;
		}

		bool TestLoopbackDuplicateFault(std::string* error) {
			LoopbackTransport host;
			LoopbackTransport client;
			LoopbackTransportConfig config;
			config.unreliableDuplicateEveryN = 1;
			client.SetFaultConfig(config);

			if (!host.StartHost(41003, error) || !client.Connect("loopback", 41003, error)) {
				return false;
			}
			host.PollEvents();
			client.PollEvents();

			if (!client.Send(1, NetTransportLane::InputUnreliable, {9}, error)) {
				return false;
			}
			std::vector<NetTransportEvent> events = host.PollEvents();
			if (events.size() != 2U || events[0].bytes != std::vector<uint8_t>{9} || events[1].bytes != std::vector<uint8_t>{9}) {
				*error = "unreliable deterministic duplicate behavior differed";
				return false;
			}

			return true;
		}

		bool TestHandshakeNameUtf8(std::string* error) {
			// Independent checks accumulate so the encode and decode refusals cannot hide each other.
			std::vector<std::string> failures;
			const std::array<std::pair<const char*, std::string>, 4> badNames{{
			    {"truncated two-byte lead", std::string("caf\xC3")},
			    {"lone continuation byte", std::string("\x80" "abc")},
			    {"byte that is never a lead", std::string("na\xFF\xFE")},
			    {"encoded surrogate", std::string("na\xED\xA0\x80")},
			}};
			for (const auto& badName: badNames) {
				NetMessage message;
				message.sequence = 1;
				NetClientHello hello = MakeClientHello();
				hello.displayName = badName.second;
				message.payload = hello;
				std::vector<uint8_t> bytes;
				NetProtocolError encodeError;
				if (NetProtocol::Encode(message, bytes, &encodeError)) {
					failures.emplace_back(std::string("the client hello encoded a display name with a ") + badName.first);
				} else if (encodeError.message != "display_name is not valid UTF-8") {
					failures.emplace_back(std::string("the client hello refused a ") + badName.first + " as: " + encodeError.message);
				}
			}
			// The joiner writes its own bytes, so the refusal has to hold on the reading side too.
			NetMessage plain;
			plain.sequence = 1;
			plain.payload = MakeClientHello();
			std::vector<uint8_t> plainBytes;
			NetProtocolError plainError;
			if (!NetProtocol::Encode(plain, plainBytes, &plainError)) {
				*error = "could not encode a plain client hello: " + plainError.message;
				return false;
			}
			const std::string needle = "Player";
			const auto found = std::search(plainBytes.begin(), plainBytes.end(), needle.begin(), needle.end());
			if (found == plainBytes.end()) {
				*error = "the client hello display name is not where this arm patches it";
				return false;
			}
			*found = 0xC3;
			const NetDecodeResult decoded = NetProtocol::Decode(plainBytes);
			if (decoded.ok) {
				failures.emplace_back("the client hello decoded a display name with a truncated two-byte lead");
			} else if (decoded.error.message != "display_name is not valid UTF-8") {
				failures.emplace_back("the decoded bad display name was refused as: " + decoded.error.message);
			}
			// Multi-byte names players actually type stay acceptable.
			NetMessage good;
			good.sequence = 1;
			NetClientHello goodHello = MakeClientHello();
			goodHello.displayName = std::string("caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC");
			good.payload = goodHello;
			std::vector<uint8_t> goodBytes;
			NetProtocolError goodError;
			if (!NetProtocol::Encode(good, goodBytes, &goodError)) {
				failures.emplace_back("the client hello refused a valid multi-byte UTF-8 display name: " + goodError.message);
			} else {
				const NetDecodeResult goodDecoded = NetProtocol::Decode(goodBytes);
				const auto* payload = goodDecoded.ok ? std::get_if<NetClientHello>(&goodDecoded.message.payload) : nullptr;
				if (!payload || payload->displayName != goodHello.displayName) {
					failures.emplace_back("a valid multi-byte UTF-8 display name did not survive the hello round trip");
				}
			}
			if (failures.empty()) {
				return true;
			}
			*error = failures.front();
			for (size_t index = 1; index < failures.size(); ++index) {
				*error += " | " + failures[index];
			}
			return false;
		}
	}

	int NetProtocolSelfTest::Run() {
		auto fail = [](const std::string& message) {
			std::cerr << "[net-protocol-selftest] FAIL: " << message << std::endl;
			return 1;
		};

		std::string error;
		if (!TestRoundTrips(&error)) {
			return fail(error);
		}
		if (!TestHandshakeNameUtf8(&error)) {
			return fail(error);
		}
		if (!TestCanonicalHeader(&error)) {
			return fail(error);
		}
		if (!TestDecodeFailures(&error)) {
			return fail(error);
		}
		if (!TestH4RoundTrips(&error)) {
			return fail(error);
		}
		if (!TestH4CanonicalBytes(&error)) {
			return fail(error);
		}
		if (!TestH4DecodeFailures(&error)) {
			return fail(error);
		}
		if (!TestModuleDigestRoundTrips(&error)) {
			return fail(error);
		}
		if (!TestModuleDigestCaps(&error)) {
			return fail(error);
		}
		if (!TestChatRoundTrips(&error)) {
			return fail(error);
		}
		if (!TestOldWireEncoding(&error)) {
			return fail(error);
		}
		if (!TestLoopback(&error)) {
			return fail(error);
		}
		if (!TestLoopbackFaults(&error)) {
			return fail(error);
		}
		if (!TestLoopbackDuplicateFault(&error)) {
			return fail(error);
		}

		std::cout << "[net-protocol-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
