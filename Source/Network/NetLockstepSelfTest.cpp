#include "NetLockstepSelfTest.h"

#include "LoopbackTransport.h"
#include "NetLockstep.h"
#include "NetProtocol.h"

#include <iostream>
#include <string>
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
			};
			if (!RoundTrip({start}, error)) {
				return false;
			}

			NetLockstepFrame frame;
			frame.senderPeerId = 2;
			frame.targetFrame = 32;
			frame.frames = {MakeFrame(100, 1), MakeFrame(200, 2)};
			frame.commands = {NetGameCommand{2, NetGameSetTeamFunds{0, 1500}}, NetGameCommand{2, NetGameSetTeamFunds{1, -250}}, NetGameCommand{2, NetGameSpawnActor{"AHuman", "Green Dummy", "Base.rte", 1234.5F, -67.25F, 1}}, NetGameCommand{2, NetGameDeliverCargo{"ACDropShip", "Dropship MK1", "Base.rte", 880.0F, 48.5F, 0, {{"AHuman", "Green Dummy", "Base.rte"}, {"AHuman", "Robot 1", "Base.rte"}}}}, NetGameCommand{2, NetGameDeliverCargo{"ACRocket", "Rocket MK2", "Base.rte", 512.0F, 300.0F, 1, {{"AHuman", "Green Dummy", "Base.rte"}}, true, 137.5F, false, 4, 600.0F, 350.25F, 424242, 1, -32.0F}}, NetGameCommand{2, NetGameScuttleCraft{17143, 0}}, NetGameCommand{2, NetGameInventoryOp{9001, 1, NetGameInventoryOp::Drop, 0, 2, true, 0.5F, -0.25F}}, NetGameCommand{2, NetGamePauseMatch{1, true}}, NetGameCommand{2, NetGamePauseMatch{0, false}}};
			if (!RoundTrip({frame}, error)) {
				return false;
			}

			if (!RoundTrip({NetLockstepAck{1, 31, 0x0000FFFFU}}, error)) {
				return false;
			}
			if (!RoundTrip({NetLockstepStop{2, NetLockstepStopReason::Complete, 120, "done"}}, error)) {
				return false;
			}
			std::array<uint8_t, 32> checksumHash{};
			for (size_t i = 0; i < checksumHash.size(); ++i) {
				checksumHash[i] = static_cast<uint8_t>(i * 7 + 3);
			}
			if (!RoundTrip({NetLockstepChecksum{1, 99, checksumHash}}, error)) {
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
				0x02, 0x00,
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
	}

	int NetLockstepSelfTest::Run() {
		auto fail = [](const std::string& message) {
			std::cerr << "[net-lockstep-selftest] FAIL: " << message << std::endl;
			return 1;
		};

		std::string error;
		if (!TestRoundTrips(&error) ||
		    !TestCanonicalHeader(&error) ||
		    !TestDecodeFailures(&error) ||
		    !TestSemanticFailures(&error) ||
		    !TestCoordinatorDelayedHappyPath(&error) ||
		    !TestCoordinatorIgnoresSessionPacketsAtHandoff(&error) ||
		    !TestCoordinatorUnreliableOutOfOrderDuplicate(&error) ||
		    !TestCoordinatorMissingFrameTimeout(&error)) {
			return fail(error);
		}

		std::cout << "[net-lockstep-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
