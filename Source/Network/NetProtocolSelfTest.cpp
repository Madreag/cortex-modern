#include "NetProtocolSelfTest.h"

#include "LoopbackTransport.h"
#include "NetProtocol.h"

#include <iostream>
#include <string>
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
				0x01, 0x00,
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
		if (!TestCanonicalHeader(&error)) {
			return fail(error);
		}
		if (!TestDecodeFailures(&error)) {
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
