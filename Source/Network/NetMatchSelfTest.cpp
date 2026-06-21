#include "NetMatchSelfTest.h"

#include "NetActorOwnership.h"
#include "GnsTransport.h"
#include "NetIdentity.h"
#include "NetLobbySession.h"
#include "NetLobbyProtocol.h"
#include "LoopbackTransport.h"
#include "NetLockstep.h"
#include "NetMatchService.h"

#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace RTE {

	namespace {
		template <typename Payload>
		bool RoundTrip(const Payload& payload, std::string* error) {
			NetLobbyMessage message;
			message.payload = payload;
			std::vector<uint8_t> bytes;
			NetLobbyError encodeError;
			if (!NetLobbyProtocol::Encode(message, bytes, &encodeError)) {
				*error = std::string("encode failed: ") + encodeError.message;
				return false;
			}
			const NetLobbyDecodeResult decoded = NetLobbyProtocol::Decode(bytes);
			if (!decoded.ok) {
				*error = std::string("decode failed: ") + decoded.error.message;
				return false;
			}
			const Payload* decodedPayload = std::get_if<Payload>(&decoded.message.payload);
			if (!decodedPayload || *decodedPayload != payload) {
				*error = "round-trip payload mismatch";
				return false;
			}
			return true;
		}

		NetMatchConfig MakeConfig() {
			NetMatchConfig config = NetMatchConfigUtil::MakeDefault(0x5048413453455353ULL);
			config.activityPreset = "Skirmish Defense";
			config.sceneName = "Grasslands";
			config.modePreset = "PvP";
			config.ownershipPolicy = NetActorOwnershipPolicy::TeamOwner;
			return config;
		}

		bool StartLoopbackTransports(uint16_t port, LoopbackTransport& hostTransport, LoopbackTransport& clientTransport, NetPeerId& hostRemotePeer, NetPeerId& clientRemotePeer, std::string* error);

		bool TestMatchConfigHashAndValidation(std::string* error) {
			NetMatchConfig config = MakeConfig();
			if (!NetMatchConfigUtil::ValidateLocalAlpha(config, error)) {
				return false;
			}
			NetMatchConfig reordered = config;
			std::swap(reordered.players[0], reordered.players[1]);
			if (NetMatchConfigUtil::HashConfig(config) != NetMatchConfigUtil::HashConfig(reordered)) {
				*error = "match config hash depends on player vector order";
				return false;
			}
			reordered.players[0].team = 3;
			if (NetMatchConfigUtil::HashConfig(config) == NetMatchConfigUtil::HashConfig(reordered)) {
				*error = "match config hash did not change after team mutation";
				return false;
			}
			config.inputDelayFrames = 2;
			if (NetMatchConfigUtil::ValidateLocalAlpha(config, nullptr)) {
				*error = "local alpha validation accepted nonzero input delay";
				return false;
			}
			NetMatchConfig invalidTeam = MakeConfig();
			invalidTeam.players[0].team = 4;
			if (NetMatchConfigUtil::ValidateLocalAlpha(invalidTeam, nullptr)) {
				*error = "local alpha validation accepted out-of-range team 4";
				return false;
			}
			return true;
		}

		bool TestOwnershipPolicies(std::string* error) {
			NetMatchConfig config = MakeConfig();
			config.ownershipPolicy = NetActorOwnershipPolicy::TeamOwner;
			if (NetActorOwnership::ResolveOwnerPeer(config, {101, 0, false}) != 1 ||
			    NetActorOwnership::ResolveOwnerPeer(config, {102, 1, false}) != 2 ||
			    NetActorOwnership::ResolveOwnerPeer(config, {103, 4, true}) != 1) {
				*error = "team-owner policy assigned the wrong peer";
				return false;
			}
			config.ownershipPolicy = NetActorOwnershipPolicy::HostCpuRemoteHuman;
			if (NetActorOwnership::ResolveOwnerPeer(config, {201, 1, false}) != 2 ||
			    NetActorOwnership::ResolveOwnerPeer(config, {202, 1, true}) != 1) {
				*error = "host-cpu-remote-human policy assigned the wrong peer";
				return false;
			}
			const NetActorOwnershipSummary summary = NetActorOwnership::Summarize(config, {{1, 0, false}, {2, 1, false}, {3, 1, true}});
			const auto hostActors = summary.actorsByPeer.find(1);
			const auto clientActors = summary.actorsByPeer.find(2);
			if (hostActors == summary.actorsByPeer.end() || clientActors == summary.actorsByPeer.end() || hostActors->second != 2 || clientActors->second != 1) {
				*error = "ownership summary counts are wrong";
				return false;
			}
			return true;
		}

		bool TestLockstepCoordinatorUsesMatchOwnership(std::string* error) {
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetPeerId hostRemotePeer = c_InvalidNetPeerId;
			NetPeerId clientRemotePeer = c_InvalidNetPeerId;
			if (!StartLoopbackTransports(43004, hostTransport, clientTransport, hostRemotePeer, clientRemotePeer, error)) {
				return false;
			}

			NetMatchConfig matchConfig = MakeConfig();
			NetLockstepConfig config;
			config.sessionId = matchConfig.sessionId;
			config.startFrame = 0;
			config.inputDelayFrames = 0;
			config.localPeerId = 1;
			config.remotePeerId = 2;
			config.peerCount = matchConfig.peerCount;
			config.remoteTransportPeerId = hostRemotePeer;
			config.scenario = matchConfig.activityPreset;
			config.ownershipPolicy = NetMatchConfigUtil::OwnershipPolicyName(matchConfig.ownershipPolicy);
			config.matchConfig = matchConfig;

			NetLockstepCoordinator hostCoordinator;
			if (!hostCoordinator.Start(hostTransport, config, error)) {
				return false;
			}
			if (!hostCoordinator.IsLocalActor(1001, 0, false)) {
				*error = "team-owner lockstep ownership did not keep team 0 local to host";
				return false;
			}
			if (hostCoordinator.IsLocalActor(1002, 1, false)) {
				*error = "team-owner lockstep ownership kept team 1 local to host";
				return false;
			}
			return true;
		}

		bool TestLobbyCodecRoundTrips(std::string* error) {
			const NetMatchConfig config = MakeConfig();
			const NetHash32 configHash = NetMatchConfigUtil::HashConfig(config);
			if (!RoundTrip(NetLobbyHello{1, 1, 1, "Host", "host"}, error) ||
			    !RoundTrip(NetLobbyPeerState{2, true, 12, 2, "Client", "windows"}, error) ||
			    !RoundTrip(NetLobbyMatchConfig{config}, error) ||
			    !RoundTrip(NetLobbyConfigAck{2, true, configHash, ""}, error) ||
			    !RoundTrip(NetLobbyReady{2, true}, error) ||
			    !RoundTrip(NetLobbyStart{config.sessionId, 120, 0, configHash}, error) ||
			    !RoundTrip(NetLobbyAbort{1, "user cancelled"}, error)) {
				return false;
			}
			NetLobbyMessage abortMessage;
			abortMessage.payload = NetLobbyAbort{2, "peer cancelled"};
			std::vector<uint8_t> bytes;
			NetLobbyError encodeError;
			if (!NetLobbyProtocol::Encode(abortMessage, bytes, &encodeError)) {
				*error = "could not encode abort payload";
				return false;
			}
			const NetLobbyDecodeResult decoded = NetLobbyProtocol::Decode(bytes);
			const NetLobbyAbort* abort = decoded.ok ? std::get_if<NetLobbyAbort>(&decoded.message.payload) : nullptr;
			if (!abort || abort->peerId != 2 || abort->reason != "peer cancelled") {
				*error = "abort payload did not preserve peer id";
				return false;
			}
			return true;
		}

		bool TestMalformedLobbyPayloads(std::string* error) {
			NetLobbyMessage message;
			message.payload = NetLobbyHello{1, 1, 1, std::string(NetLobbyProtocol::c_MaxDisplayNameBytes + 1, 'x'), "host"};
			std::vector<uint8_t> bytes;
			NetLobbyError encodeError;
			if (NetLobbyProtocol::Encode(message, bytes, &encodeError) || encodeError.code != NetLobbyErrorCode::StringTooLong) {
				*error = "oversized display name was not rejected";
				return false;
			}

			message.payload = NetLobbyReady{1, true};
			if (!NetLobbyProtocol::Encode(message, bytes, &encodeError)) {
				*error = "could not encode ready payload";
				return false;
			}
			bytes[0] = 0;
			const NetLobbyDecodeResult badMagic = NetLobbyProtocol::Decode(bytes);
			if (badMagic.ok || badMagic.error.code != NetLobbyErrorCode::BadMagic) {
				*error = "bad magic was not rejected";
				return false;
			}
			bytes = {1, 2, 3};
			const NetLobbyDecodeResult shortHeader = NetLobbyProtocol::Decode(bytes);
			if (shortHeader.ok || shortHeader.error.code != NetLobbyErrorCode::ShortHeader) {
				*error = "short header was not rejected";
				return false;
			}
			return true;
		}

		bool CaptureLoopbackPeers(LoopbackTransport& hostTransport, LoopbackTransport& clientTransport, NetPeerId& hostRemotePeer, NetPeerId& clientRemotePeer, std::string* error) {
			hostRemotePeer = c_InvalidNetPeerId;
			clientRemotePeer = c_InvalidNetPeerId;
			for (const NetTransportEvent& event : hostTransport.PollEvents()) {
				if (event.type == NetTransportEventType::PeerConnected) {
					hostRemotePeer = event.peerId;
				}
			}
			for (const NetTransportEvent& event : clientTransport.PollEvents()) {
				if (event.type == NetTransportEventType::PeerConnected) {
					clientRemotePeer = event.peerId;
				}
			}
			if (hostRemotePeer == c_InvalidNetPeerId || clientRemotePeer == c_InvalidNetPeerId) {
				*error = "loopback peer connection events were not observed";
				return false;
			}
			return true;
		}

		bool StartLoopbackTransports(uint16_t port, LoopbackTransport& hostTransport, LoopbackTransport& clientTransport, NetPeerId& hostRemotePeer, NetPeerId& clientRemotePeer, std::string* error) {
			if (!hostTransport.StartHost(port, error) || !clientTransport.Connect("loopback", port, error)) {
				return false;
			}
			return CaptureLoopbackPeers(hostTransport, clientTransport, hostRemotePeer, clientRemotePeer, error);
		}

		bool DriveLobbyPair(LoopbackTransport& hostTransport, LoopbackTransport& clientTransport, NetLobbySession& hostLobby, NetLobbySession& clientLobby, std::string* error, uint64_t startNow = 0) {
			for (uint64_t now = startNow; now <= startNow + 1000; now += 10) {
				hostLobby.Tick(now);
				clientLobby.Tick(now);
				if (hostLobby.IsStarted() && clientLobby.IsStarted()) {
					return true;
				}
				if (hostLobby.IsFailed() || hostLobby.IsRejected() || clientLobby.IsFailed() || clientLobby.IsRejected()) {
					*error = "lobby failed; host=" + std::string(NetLobbySession::StateName(hostLobby.GetState())) +
					         " client=" + NetLobbySession::StateName(clientLobby.GetState());
					return false;
				}
				hostTransport.AdvanceTimeMs(10);
				clientTransport.AdvanceTimeMs(10);
			}
			*error = "lobby did not reach Started";
			return false;
		}

		bool TestLobbyStateMachineHappyPath(std::string* error) {
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetPeerId hostRemotePeer = c_InvalidNetPeerId;
			NetPeerId clientRemotePeer = c_InvalidNetPeerId;
			if (!StartLoopbackTransports(43001, hostTransport, clientTransport, hostRemotePeer, clientRemotePeer, error)) {
				return false;
			}

			const NetMatchConfig matchConfig = MakeConfig();
			NetLobbySession hostLobby;
			NetLobbySession clientLobby;
			NetLobbySessionConfig hostConfig;
			hostConfig.host = true;
			hostConfig.localPeerId = 1;
			hostConfig.remotePeerId = 2;
			hostConfig.remoteTransportPeerId = hostRemotePeer;
			hostConfig.matchConfig = matchConfig;
			hostConfig.startFrame = 77;
			hostConfig.displayName = "Host";
			hostConfig.platform = "windows";
			hostConfig.peerStateIntervalMs = 10; // exchange names during the handshake (Started is terminal)
			NetLobbySessionConfig clientConfig = hostConfig;
			clientConfig.host = false;
			clientConfig.localPeerId = 2;
			clientConfig.remotePeerId = 1;
			clientConfig.remoteTransportPeerId = clientRemotePeer;
			clientConfig.displayName = "Client";
			if (!hostLobby.Start(hostTransport, hostConfig, error) || !clientLobby.Start(clientTransport, clientConfig, error)) {
				return false;
			}
			if (!DriveLobbyPair(hostTransport, clientTransport, hostLobby, clientLobby, error)) {
				return false;
			}
			if (hostLobby.GetMatchConfigHash() != clientLobby.GetMatchConfigHash() || hostLobby.GetStartFrame() != 77 || clientLobby.GetStartFrame() != 77) {
				*error = "lobby start did not preserve match hash/start frame";
				return false;
			}
			if (hostLobby.GetRemoteName() != "Client" || clientLobby.GetRemoteName() != "Host") {
				*error = "lobby did not exchange peer display names (host='" + hostLobby.GetRemoteName() + "' client='" + clientLobby.GetRemoteName() + "')";
				return false;
			}
			const std::string report = hostLobby.BuildReportJson();
			if (report.find("\"match_config_hash\"") == std::string::npos || report.find("\"ownership_policy\"") == std::string::npos) {
				*error = "lobby report is missing match config diagnostics";
				return false;
			}
			return true;
		}

		bool TestLobbyManualReadyStart(std::string* error) {
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetPeerId hostRemotePeer = c_InvalidNetPeerId;
			NetPeerId clientRemotePeer = c_InvalidNetPeerId;
			if (!StartLoopbackTransports(43003, hostTransport, clientTransport, hostRemotePeer, clientRemotePeer, error)) {
				return false;
			}

			NetLobbySession hostLobby;
			NetLobbySession clientLobby;
			NetLobbySessionConfig hostConfig;
			hostConfig.host = true;
			hostConfig.localPeerId = 1;
			hostConfig.remotePeerId = 2;
			hostConfig.remoteTransportPeerId = hostRemotePeer;
			hostConfig.matchConfig = MakeConfig();
			hostConfig.startFrame = 0;
			hostConfig.autoStart = false;
			NetLobbySessionConfig clientConfig = hostConfig;
			clientConfig.host = false;
			clientConfig.localPeerId = 2;
			clientConfig.remotePeerId = 1;
			clientConfig.remoteTransportPeerId = clientRemotePeer;
			clientConfig.autoReady = false;
			if (!hostLobby.Start(hostTransport, hostConfig, error) || !clientLobby.Start(clientTransport, clientConfig, error)) {
				return false;
			}

			for (uint64_t now = 0; now <= 200; now += 10) {
				hostLobby.Tick(now);
				clientLobby.Tick(now);
				hostTransport.AdvanceTimeMs(10);
				clientTransport.AdvanceTimeMs(10);
			}
			if (hostLobby.IsStarted() || clientLobby.IsStarted()) {
				*error = "manual lobby started before Ready/Start";
				return false;
			}
			clientLobby.SetLocalReady(true);
			for (uint64_t now = 210; now <= 400; now += 10) {
				hostLobby.Tick(now);
				clientLobby.Tick(now);
				hostTransport.AdvanceTimeMs(10);
				clientTransport.AdvanceTimeMs(10);
			}
			if (hostLobby.IsStarted() || clientLobby.IsStarted()) {
				*error = "manual lobby started before host Start";
				return false;
			}
			hostLobby.RequestStart();
			if (!DriveLobbyPair(hostTransport, clientTransport, hostLobby, clientLobby, error)) {
				return false;
			}
			if (hostLobby.GetStartFrame() != 0 || clientLobby.GetStartFrame() != 0) {
				*error = "manual lobby did not preserve activity start frame 0";
				return false;
			}
			return true;
		}

		bool TestLobbyManualReadyCanWait(std::string* error) {
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetPeerId hostRemotePeer = c_InvalidNetPeerId;
			NetPeerId clientRemotePeer = c_InvalidNetPeerId;
			if (!StartLoopbackTransports(43005, hostTransport, clientTransport, hostRemotePeer, clientRemotePeer, error)) {
				return false;
			}

			NetLobbySession hostLobby;
			NetLobbySession clientLobby;
			NetLobbySessionConfig hostConfig;
			hostConfig.host = true;
			hostConfig.localPeerId = 1;
			hostConfig.remotePeerId = 2;
			hostConfig.remoteTransportPeerId = hostRemotePeer;
			hostConfig.matchConfig = MakeConfig();
			hostConfig.autoStart = false;
			NetLobbySessionConfig clientConfig = hostConfig;
			clientConfig.host = false;
			clientConfig.localPeerId = 2;
			clientConfig.remotePeerId = 1;
			clientConfig.remoteTransportPeerId = clientRemotePeer;
			clientConfig.autoReady = false;
			if (!hostLobby.Start(hostTransport, hostConfig, error) || !clientLobby.Start(clientTransport, clientConfig, error)) {
				return false;
			}

			for (uint64_t now = 0; now <= 7000; now += 100) {
				hostLobby.Tick(now);
				clientLobby.Tick(now);
				if (hostLobby.IsFailed() || clientLobby.IsFailed()) {
					*error = "manual lobby timed out while waiting for human Ready";
					return false;
				}
				hostTransport.AdvanceTimeMs(100);
				clientTransport.AdvanceTimeMs(100);
			}
			clientLobby.SetLocalReady(true);
			hostLobby.RequestStart();
			return DriveLobbyPair(hostTransport, clientTransport, hostLobby, clientLobby, error, 7010);
		}

		bool TestLobbyReadyDoesNotStartBeforeConfigAck(std::string* error) {
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetPeerId hostRemotePeer = c_InvalidNetPeerId;
			NetPeerId clientRemotePeer = c_InvalidNetPeerId;
			if (!StartLoopbackTransports(43002, hostTransport, clientTransport, hostRemotePeer, clientRemotePeer, error)) {
				return false;
			}

			NetLobbySession hostLobby;
			NetLobbySessionConfig hostConfig;
			hostConfig.host = true;
			hostConfig.localPeerId = 1;
			hostConfig.remotePeerId = 2;
			hostConfig.remoteTransportPeerId = hostRemotePeer;
			hostConfig.matchConfig = MakeConfig();
			hostConfig.startFrame = 88;
			if (!hostLobby.Start(hostTransport, hostConfig, error)) {
				return false;
			}

			NetLobbyMessage readyMessage;
			readyMessage.payload = NetLobbyReady{2, true};
			std::vector<uint8_t> readyBytes;
			NetLobbyError encodeError;
			if (!NetLobbyProtocol::Encode(readyMessage, readyBytes, &encodeError) ||
			    !clientTransport.Send(clientRemotePeer, NetTransportLane::ControlReliable, readyBytes, error)) {
				*error = "could not send early Ready message";
				return false;
			}
			hostLobby.Tick(0);
			if (hostLobby.IsStarted()) {
				*error = "host lobby started before config ack";
				return false;
			}
			if (hostLobby.GetState() != NetLobbyState::WaitingForConfigAck) {
				*error = "early Ready moved host lobby to unexpected state";
				return false;
			}
			return true;
		}

		bool TestServiceRuntimeErrorSurface(std::string* error) {
			NetMatchService service;
			service.ReportRuntimeError("PeerDisconnected:test");
			if (service.GetState() != NetMatchServiceState::Failed ||
			    service.GetStatusText() != "Match stopped" ||
			    service.GetErrorText() != "PeerDisconnected:test") {
				*error = "runtime error was not visible through match service state";
				return false;
			}
			return true;
		}
	}

	int NetMatchSelfTest::Run() {
		auto fail = [](const std::string& message) {
			std::cerr << "[net-match-selftest] FAIL: " << message << std::endl;
			return 1;
		};

		std::string error;
		if (!TestMatchConfigHashAndValidation(&error)) return fail(error);
		if (!TestOwnershipPolicies(&error)) return fail(error);
		if (!TestLockstepCoordinatorUsesMatchOwnership(&error)) return fail(error);
		if (!TestLobbyCodecRoundTrips(&error)) return fail(error);
		if (!TestMalformedLobbyPayloads(&error)) return fail(error);
		if (!TestLobbyStateMachineHappyPath(&error)) return fail(error);
		if (!TestLobbyManualReadyStart(&error)) return fail(error);
		if (!TestLobbyManualReadyCanWait(&error)) return fail(error);
		if (!TestLobbyReadyDoesNotStartBeforeConfigAck(&error)) return fail(error);
		if (!TestServiceRuntimeErrorSurface(&error)) return fail(error);

		std::cout << "[net-match-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
