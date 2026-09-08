#include "NetLockstepSelfTest.h"

#include "LoopbackTransport.h"
#include "NetLockstep.h"
#include "NetProtocol.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
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
			frame.commands = {NetGameCommand{2, NetGameSetTeamFunds{0, 1500}}, NetGameCommand{2, NetGameSetTeamFunds{1, -250}}, NetGameCommand{2, NetGameSpawnActor{"AHuman", "Green Dummy", "Base.rte", 1234.5F, -67.25F, 1}}, NetGameCommand{2, NetGameDeliverCargo{"ACDropShip", "Dropship MK1", "Base.rte", 880.0F, 48.5F, 0, {{"AHuman", "Green Dummy", "Base.rte"}, {"AHuman", "Robot 1", "Base.rte"}}}}, NetGameCommand{2, NetGameDeliverCargo{"ACRocket", "Rocket MK2", "Base.rte", 512.0F, 300.0F, 1, {{"AHuman", "Green Dummy", "Base.rte"}}, true, 137.5F, false, 4, 600.0F, 350.25F, 424242, 1, -32.0F}}, NetGameCommand{2, NetGameScuttleCraft{17143, 0}}, NetGameCommand{2, NetGameInventoryOp{9001, 1, NetGameInventoryOp::Drop, 0, 2, true, 0.5F, -0.25F}}, NetGameCommand{2, NetGamePauseMatch{1, true}}, NetGameCommand{2, NetGamePauseMatch{0, false}}, NetGameCommand{2, NetGameSetActorAIMode{31337, 1, 6}}, NetGameCommand{2, NetGameSwitchControl{41414, 0, 2}}, NetGameCommand{2, NetGameAIEquip{51515, 1, NetGameAIEquip::LoadedFirearmInGroup, false, "Weapons - Primary", "Weapons - Explosive", "", ""}}, NetGameCommand{2, NetGameAIEquip{51516, 0, NetGameAIEquip::NamedDevice, false, "", "", "Base.rte", "Battle Rifle"}}, NetGameCommand{2, NetGameAIEquip{51517, 1, NetGameAIEquip::ShieldInBGArm, true, "", "", "", ""}}, NetGameCommand{2, NetGameAIEquip{51518, 0, NetGameAIEquip::UnequipFGArm, false, "", "", "", ""}}, NetGameCommand{2, NetGameAIOrder{61616, 0, NetGameAIOrder::FormSquad, 512.5F, -12.25F, 61617}}, NetGameCommand{2, NetGameAIOrder{61618, 1, NetGameAIOrder::MOWaypoint, 0.0F, 0.0F, 61616}}, NetGameCommand{2, NetGameSoundOp{71717, 1, 0x00FF00FF00FF0001ULL, NetGameSoundOp::Play, 0, 3, 0, 0.0F, 0.0F, {}}}, NetGameCommand{2, NetGameSoundOp{71718, 0, 0x0000000000000002ULL, NetGameSoundOp::SetProperty, 13, -1, 0, -12.5F, 88.25F, {}}}, NetGameCommand{2, NetGameSoundOp{71719, 1, 0x0000000000000003ULL, NetGameSoundOp::SelectSounds, 0, -1, 0, 0.0F, 0.0F, {2, 0, 7}}}, NetGameCommand{2, NetGameSoundOp{71720, 0, 0x0000000000000004ULL, NetGameSoundOp::FadeOut, 0, -1, 250, 0.0F, 0.0F, {}}}};
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
				0x0D, 0x00,
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
				if (outState == NetLockstepState::Running && !cleanLeave && !fenceTransport) {
					// The window closes: the very next Tick must end a round nobody is coming back to.
					stub.held = false;
					host.Tick(now + 5);
					outState = host.GetState();
					outReason = host.GetStats().timeoutReason;
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
		    !TestCoordinatorAdjudicatedPeerKeepsItsSeat(&error)) {
			return fail(error);
		}

		std::cout << "[net-lockstep-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
