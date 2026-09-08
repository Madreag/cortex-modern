#include "NetMatchSelfTest.h"

#include "NetActorOwnership.h"
#include "GnsTransport.h"
#include "NetIdentity.h"
#include "NetLobbySession.h"
#include "NetLobbyProtocol.h"
#include "LoopbackTransport.h"
#include "NetLockstep.h"
#include "NetMatchReplay.h"
#include "NetMatchService.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <iterator>
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

		bool TestReplayCommandSenders(std::string* error) {
			const auto directory = std::filesystem::temp_directory_path() / ("cc-replay-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			if (!std::filesystem::create_directory(directory)) {
				*error = "could not create replay test directory";
				return false;
			}
			const auto path = directory / "match.ccreplay";
			struct Cleanup {
				std::filesystem::path path;
				~Cleanup() {
					std::error_code ignored;
					std::filesystem::remove(path, ignored);
					std::filesystem::remove(path.parent_path(), ignored);
				}
			} cleanup{path};
			ControllerFrame controller;
			controller.actorUniqueID = 123;
			NetLockstepFrame first{1, 43, {controller}, {{1, NetGameSetTeamFunds{0, 1234}}, {2, NetGameSpawnActor{"AHuman", "Brain Robot", "Base.rte", 500, 600, 1}}}};
			NetLockstepFrame second{1, 44, {}, {}};
			NetMatchReplayWriter writer;
			if (!writer.Open(path.string(), MakeConfig(), error)) return false;
			std::string refusal;
			if (writer.WriteFrame(42, {}, {{0, NetGameSetTeamFunds{0, 1}}}, &refusal) || refusal.empty()) {
				*error = "replay writer accepted an invalid command sender";
				return false;
			}
			if (!writer.WriteFrame(first.targetFrame, first.frames, first.commands, error) ||
			    !writer.WriteFrame(second.targetFrame, second.frames, second.commands, error)) return false;
			writer.Close();
			NetMatchReplayReader reader;
			if (!reader.Open(path.string(), error)) return false;
			NetLockstepFrame decoded;
			bool eof = false;
			if (reader.GetVersion() != 5 || reader.GetStartFrame() != 43 ||
			    !reader.ReadFrame(decoded, eof, error) || decoded != first ||
			    !reader.ReadFrame(decoded, eof, error) || decoded != second ||
			    reader.ReadFrame(decoded, eof, error) || !eof) {
				*error = "replay round-trip changed the commands, controllers or tick range";
				return false;
			}
			reader.Close();
			NetReplayVerifyReport report;
			if (!NetMatchReplayReader::Verify(path.string(), report) || report.frames != 2 || report.firstFrame != 43 || report.lastFrame != 44) {
				*error = "replay integrity scan failed the round-trip";
				return false;
			}
			std::ifstream input(path, std::ios::binary);
			const std::vector<uint8_t> original{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
			input.close();
			auto read32 = [](const std::vector<uint8_t>& bytes, size_t offset) {
				return uint32_t(bytes.at(offset)) | (uint32_t(bytes.at(offset + 1)) << 8) | (uint32_t(bytes.at(offset + 2)) << 16) | (uint32_t(bytes.at(offset + 3)) << 24);
			};
			auto write32 = [](std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
				for (size_t i = 0; i < 4; ++i) bytes.at(offset + i) = static_cast<uint8_t>(value >> (8 * i));
			};
			const size_t recordOffset = 12 + read32(original, 8);
			const size_t payloadOffset = recordOffset + 8;
			const size_t payloadLength = read32(original, recordOffset);
			const size_t wireLength = read32(original, payloadOffset);
			const size_t senderOffset = payloadOffset + 4 + wireLength;
			auto writeFile = [&](const std::vector<uint8_t>& bytes) {
				std::ofstream output(path, std::ios::binary | std::ios::trunc);
				output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
				return output.good();
			};
			auto checksum = [&](std::vector<uint8_t>& bytes) {
				const size_t length = read32(bytes, recordOffset);
				const std::vector<uint8_t> payload(bytes.begin() + payloadOffset, bytes.begin() + payloadOffset + length);
				write32(bytes, recordOffset + 4, ControllerFrameCodec::PayloadChecksum(payload));
			};
			auto rejects = [&](const std::vector<uint8_t>& bytes, const char* expectedError) {
				if (!writeFile(bytes) || NetMatchReplayReader::Verify(path.string(), report) || report.error.find(expectedError) == std::string::npos) {
					*error = "replay corruption check missed " + std::string(expectedError) + ": " + report.error;
					return false;
				}
				return true;
			};
			auto bytes = original;
			bytes[16] = 2;
			const std::vector<uint8_t> legacyConfig(bytes.begin() + 12, bytes.begin() + recordOffset);
			if (NetLobbyProtocol::Decode(legacyConfig).ok || !writeFile(bytes) || !reader.Open(path.string(), error) ||
			    reader.GetConfig() != MakeConfig() || !reader.ReadFrame(decoded, eof, error) || decoded != first) {
				*error = "legacy recorded config failed or was accepted on the live wire";
				return false;
			}
			reader.Close();
			bytes[16] = 255;
			if (!rejects(bytes, "unsupported lobby protocol version")) return false;
			bytes = original;
			bytes[senderOffset + 1] = 1;
			if (!rejects(bytes, "checksum")) return false;
			for (const uint8_t sender : {uint8_t(0), uint8_t(NetLockstepCodec::c_MaxPeerCount + 1)}) {
				bytes = original;
				bytes[senderOffset + 1] = sender;
				checksum(bytes);
				if (!rejects(bytes, "command sender")) return false;
			}
			bytes = original;
			bytes.erase(bytes.begin() + senderOffset);
			write32(bytes, recordOffset, static_cast<uint32_t>(payloadLength - 1));
			checksum(bytes);
			if (!rejects(bytes, "sender count")) return false;
			bytes = original;
			write32(bytes, payloadOffset, 0xFFFFFFFFU);
			checksum(bytes);
			if (!rejects(bytes, "wire frame length")) return false;

			// Version 4 has one authenticated sender for the entire record.
			bytes.assign(original.begin(), original.begin() + payloadOffset);
			bytes[4] = 4;
			bytes.insert(bytes.end(), original.begin() + payloadOffset + 4, original.begin() + senderOffset);
			bytes.insert(bytes.end(), 4, 0xFF);
			write32(bytes, recordOffset, static_cast<uint32_t>(wireLength));
			checksum(bytes);
			if (!writeFile(bytes) || !reader.Open(path.string(), error) || !reader.ReadFrame(decoded, eof, error)) return false;
			first.commands[1].senderPeerId = 1;
			if (reader.GetVersion() != 4 || decoded != first || reader.ReadFrame(decoded, eof, error) || !eof) {
				*error = "legacy replay decoding semantics changed";
				return false;
			}
			return true;
		}

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
			if (!NetMatchConfigUtil::ValidateLocalAlpha(config, nullptr)) {
				*error = "validation rejected a valid nonzero input delay";
				return false;
			}
			config.inputDelayFrames = NetMatchConfigUtil::c_MaxInputDelayFrames + 1;
			if (NetMatchConfigUtil::ValidateLocalAlpha(config, nullptr)) {
				*error = "validation accepted an out-of-range input delay";
				return false;
			}
			config.inputDelayFrames = 0;
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
			// A team's economy-command authority is its human peer, or the host for an unassigned team.
			NetMatchConfig authorityConfig = MakeConfig();
			if (NetActorOwnership::ResolveTeamCommandAuthority(authorityConfig, 0) != 1 ||
			    NetActorOwnership::ResolveTeamCommandAuthority(authorityConfig, 1) != 2 ||
			    NetActorOwnership::ResolveTeamCommandAuthority(authorityConfig, 2) != authorityConfig.hostPeerId) {
				*error = "team command authority resolved the wrong peer";
				return false;
			}
			// A shared co-op team authorizes EVERY one of its human peers; outsiders and the CPU
			// team's non-host peers stay rejected.
			NetMatchConfig coopConfig = MakeConfig();
			coopConfig.peerCount = 3;
			coopConfig.players = {
			    NetMatchPlayerSlot{1, 0, false, "Host"},
			    NetMatchPlayerSlot{2, 0, false, "Client A"},
			    NetMatchPlayerSlot{3, 2, false, "Client B"},
			    NetMatchPlayerSlot{0, 1, true, "CPU"},
			};
			if (!NetActorOwnership::IsTeamCommandAuthority(coopConfig, 0, 1) ||
			    !NetActorOwnership::IsTeamCommandAuthority(coopConfig, 0, 2) ||
			    NetActorOwnership::IsTeamCommandAuthority(coopConfig, 0, 3) ||
			    !NetActorOwnership::IsTeamCommandAuthority(coopConfig, 1, coopConfig.hostPeerId) ||
			    NetActorOwnership::IsTeamCommandAuthority(coopConfig, 1, 2) ||
			    !NetActorOwnership::IsTeamCommandAuthority(coopConfig, 2, 3)) {
				*error = "shared-team command authority resolved wrong";
				return false;
			}
			NetMatchConfig emptyConfig = authorityConfig;
			emptyConfig.players.clear();
			if (NetActorOwnership::ResolveTeamCommandAuthority(emptyConfig, 0) != 0) {
				*error = "team command authority should be unenforced when no teams are defined";
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
			    !RoundTrip(NetLobbyPeerState{2, false, 0, 0, "Client", "windows", false}, error) ||
			    !RoundTrip(NetLobbyMatchConfig{config}, error) ||
			    !RoundTrip(NetLobbyConfigAck{2, true, configHash, ""}, error) ||
			    !RoundTrip(NetLobbyReady{2, true}, error) ||
			    !RoundTrip(NetLobbyStart{config.sessionId, 120, 0, configHash}, error) ||
			    !RoundTrip(NetLobbyAbort{1, "user cancelled"}, error)) {
				return false;
			}
			// A config carrying per-sender delays must survive the wire unchanged.
			NetMatchConfig perSender = MakeConfig();
			perSender.inputDelayFrames = 1;
			perSender.peerInputDelayFrames = {1, 7};
			if (!RoundTrip(NetLobbyMatchConfig{perSender}, error)) {
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

		bool TestLobbyStateTransfer(std::string* error) {
			// Codec round-trip first.
			NetLobbyStateChunk chunk;
			chunk.transferId = 0xABCDEF0123456789ULL;
			chunk.totalBytes = 100000;
			chunk.chunkIndex = 1;
			chunk.chunkCount = 3;
			chunk.bytes.resize(1024);
			for (size_t i = 0; i < chunk.bytes.size(); ++i) {
				chunk.bytes[i] = static_cast<uint8_t>(i * 31 + 7);
			}
			if (!RoundTrip(chunk, error)) {
				return false;
			}

			// A ~100KB state must stream host->client during the lobby round, complete BEFORE the
			// Start lands (same ordered lane), and reassemble byte-identical.
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetPeerId hostRemotePeer = c_InvalidNetPeerId;
			NetPeerId clientRemotePeer = c_InvalidNetPeerId;
			if (!StartLoopbackTransports(43008, hostTransport, clientTransport, hostRemotePeer, clientRemotePeer, error)) {
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
			NetLobbySessionConfig clientConfig = hostConfig;
			clientConfig.host = false;
			clientConfig.localPeerId = 2;
			clientConfig.remotePeerId = 1;
			clientConfig.remoteTransportPeerId = clientRemotePeer;
			if (!hostLobby.Start(hostTransport, hostConfig, error) || !clientLobby.Start(clientTransport, clientConfig, error)) {
				return false;
			}
			std::vector<uint8_t> stateBytes(100000);
			for (size_t i = 0; i < stateBytes.size(); ++i) {
				stateBytes[i] = static_cast<uint8_t>((i * 131) ^ (i >> 8));
			}
			hostLobby.BeginStateTransfer(stateBytes);
			if (!DriveLobbyPair(hostTransport, clientTransport, hostLobby, clientLobby, error)) {
				return false;
			}
			// Started implies the ordered lane already delivered every chunk.
			if (!clientLobby.HasCompleteStateTransfer()) {
				*error = "client lobby Started without the complete state transfer";
				return false;
			}
			if (clientLobby.TakeReceivedState() != stateBytes) {
				*error = "received state differs from the sent state";
				return false;
			}
			return true;
		}

		bool TestLobbyThreePeer(std::string* error) {
			const uint16_t port = 43007;
			LoopbackTransport hostT, clientAT, clientBT;
			if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) || !clientBT.Connect("loopback", port, error)) {
				return false;
			}
			// Client A connected first (host-side transport id 1), client B second (id 2). Clients see
			// the host as transport id 1. Peers: host=1, clientA=2, clientB=3.
			NetMatchConfig matchConfig = MakeConfig();
			matchConfig.peerCount = 3;
			matchConfig.players.push_back(NetMatchPlayerSlot{3, 2, false, "Client B"});
			auto cfg = [&](bool host, uint8_t local, std::map<uint8_t, NetPeerId> transports, const char* name) {
				NetLobbySessionConfig c;
				c.host = host;
				c.localPeerId = local;
				c.remoteTransportPeerIds = std::move(transports);
				c.matchConfig = matchConfig;
				c.startFrame = 5;
				c.displayName = name;
				c.platform = "windows";
				c.peerStateIntervalMs = 10;
				return c;
			};
			NetLobbySession host, clientA, clientB;
			if (!host.Start(hostT, cfg(true, 1, {{2, 1}, {3, 2}}, "Host"), error) ||
			    !clientA.Start(clientAT, cfg(false, 2, {{1, 1}}, "Client A"), error) ||
			    !clientB.Start(clientBT, cfg(false, 3, {{1, 1}}, "Client B"), error)) {
				return false;
			}
			for (uint64_t now = 0; now <= 2000; now += 10) {
				host.Tick(now);
				clientA.Tick(now);
				clientB.Tick(now);
				if (host.IsStarted() && clientA.IsStarted() && clientB.IsStarted()) {
					break;
				}
				if (host.IsFailed() || host.IsRejected() || clientA.IsFailed() || clientA.IsRejected() || clientB.IsFailed() || clientB.IsRejected()) {
					*error = "three-peer lobby failed; host=" + std::string(NetLobbySession::StateName(host.GetState())) +
					         " a=" + NetLobbySession::StateName(clientA.GetState()) + " b=" + NetLobbySession::StateName(clientB.GetState());
					return false;
				}
				hostT.AdvanceTimeMs(10);
				clientAT.AdvanceTimeMs(10);
				clientBT.AdvanceTimeMs(10);
			}
			if (!host.IsStarted() || !clientA.IsStarted() || !clientB.IsStarted()) {
				*error = "three-peer lobby did not reach Started on every peer";
				return false;
			}
			// The host waited for BOTH clients' acks + readies; all adopt the same config + start frame.
			if (host.GetMatchConfigHash() != clientA.GetMatchConfigHash() || host.GetMatchConfigHash() != clientB.GetMatchConfigHash() ||
			    clientA.GetStartFrame() != 5 || clientB.GetStartFrame() != 5) {
				*error = "three-peer lobby did not converge on the config and start frame";
				return false;
			}
			if (host.GetRemoteName(2) != "Client A" || host.GetRemoteName(3) != "Client B" || !host.IsRemoteReady(2) || !host.IsRemoteReady(3)) {
				*error = "host lobby did not track both clients' names and readies";
				return false;
			}
			return true;
		}

		// A joiner arriving into a lobby that is already full of names hears the host's roster before
		// the host's next config resend: the state names peers its placeholder config cannot seat yet.
		// Failing the session over a name-and-ping message killed every four-member lobby lane, so the
		// unseated peer is dropped and the config that follows brings it back.
		bool TestLobbyLateJoinerRosterRace(std::string* error) {
			const uint16_t port = 43008;
			NetMatchConfig matchConfig = MakeConfig();
			matchConfig.peerCount = 4;
			matchConfig.players.push_back(NetMatchPlayerSlot{3, 2, false, "Client B"});
			matchConfig.players.push_back(NetMatchPlayerSlot{4, 3, false, "Client C"});
			LoopbackTransport hostT, clientAT, clientBT, clientCT;
			if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) || !clientBT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](bool host, uint8_t local, std::map<uint8_t, NetPeerId> transports, const char* name, const NetMatchConfig& config) {
				NetLobbySessionConfig c;
				c.host = host;
				c.localPeerId = local;
				c.remoteTransportPeerIds = std::move(transports);
				c.matchConfig = config;
				c.startFrame = 5;
				c.displayName = name;
				c.platform = "windows";
				// The roster moves on every change; the config only resends on its own cadence, which is
				// exactly the gap a late joiner lands in.
				c.peerStateIntervalMs = 10;
				c.resendIntervalMs = 400;
				return c;
			};
			NetLobbySession host, clientA, clientB, clientC;
			if (!host.Start(hostT, cfg(true, 1, {{2, 1}, {3, 2}, {4, 3}}, "Host", matchConfig), error) ||
			    !clientA.Start(clientAT, cfg(false, 2, {{1, 1}}, "Client A", matchConfig), error) ||
			    !clientB.Start(clientBT, cfg(false, 3, {{1, 1}}, "Client B", matchConfig), error)) {
				return false;
			}
			auto step = [&](uint64_t now, bool withC) {
				host.Tick(now);
				clientA.Tick(now);
				clientB.Tick(now);
				if (withC) clientC.Tick(now);
				hostT.AdvanceTimeMs(10);
				clientAT.AdvanceTimeMs(10);
				clientBT.AdvanceTimeMs(10);
				if (withC) clientCT.AdvanceTimeMs(10);
			};
			// Let the roster fill up with the two seated clients first.
			uint64_t now = 0;
			for (; now <= 600; now += 10) {
				step(now, false);
			}
			if (host.GetRemoteName(2) != "Client A" || host.GetRemoteName(3) != "Client B") {
				*error = "the four-member lobby did not seat its first two clients";
				return false;
			}
			// Client C joins knowing only itself and the host, as a client does before any config lands.
			NetMatchConfig placeholder = MakeConfig();
			if (!clientCT.Connect("loopback", port, error) ||
			    !clientC.Start(clientCT, cfg(false, 4, {{1, 1}}, "Client C", placeholder), error)) {
				return false;
			}
			for (; now <= 4000; now += 10) {
				step(now, true);
				if (host.IsStarted() && clientA.IsStarted() && clientB.IsStarted() && clientC.IsStarted()) {
					break;
				}
				if (clientC.IsFailed() || clientC.IsRejected()) {
					*error = "the late joiner failed on the host's roster: " + clientC.GetFailureReason();
					return false;
				}
			}
			if (!clientC.IsStarted() || !host.IsStarted()) {
				*error = "the four-member lobby did not start with a late joiner; c=" +
				         std::string(NetLobbySession::StateName(clientC.GetState())) +
				         " host=" + NetLobbySession::StateName(host.GetState());
				return false;
			}
			// It really did see a state it could not seat - otherwise this proves nothing.
			if (clientC.GetStats().unconfiguredPeerStates == 0) {
				*error = "the late joiner never met the roster race this test exists for";
				return false;
			}
			if (host.GetMatchConfigHash() != clientC.GetMatchConfigHash()) {
				*error = "the late joiner did not adopt the host's four-member config";
				return false;
			}
			if (clientC.GetRemoteName(2) != "Client A" || clientC.GetRemoteName(3) != "Client B") {
				*error = "the late joiner did not end up with the whole roster";
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
		if (!TestReplayCommandSenders(&error)) return fail(error);
		if (!TestOwnershipPolicies(&error)) return fail(error);
		if (!TestLockstepCoordinatorUsesMatchOwnership(&error)) return fail(error);
		if (!TestLobbyCodecRoundTrips(&error)) return fail(error);
		if (!TestMalformedLobbyPayloads(&error)) return fail(error);
		if (!TestLobbyStateMachineHappyPath(&error)) return fail(error);
		if (!TestLobbyManualReadyStart(&error)) return fail(error);
		if (!TestLobbyManualReadyCanWait(&error)) return fail(error);
		if (!TestLobbyReadyDoesNotStartBeforeConfigAck(&error)) return fail(error);
		if (!TestLobbyStateTransfer(&error)) return fail(error);
		if (!TestLobbyThreePeer(&error)) return fail(error);
		if (!TestLobbyLateJoinerRosterRace(&error)) return fail(error);
		if (!TestServiceRuntimeErrorSurface(&error)) return fail(error);

		std::cout << "[net-match-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
