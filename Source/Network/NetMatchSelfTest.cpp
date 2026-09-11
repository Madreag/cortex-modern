#include "NetMatchSelfTest.h"

#include "NetActorOwnership.h"
#include "GnsTransport.h"
#include "NetIdentity.h"
#include "NetLobbySession.h"
#include "NetLobbyProtocol.h"
#include "LoopbackTransport.h"
#include "NetLockstep.h"
#include "NetMatchReplay.h"
#include "NetMatchRunner.h"
#include "NetMatchService.h"
#include "ScenarioRunner.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
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

		bool TestFailedReportKeepsAdmissionCounters(std::string* error) {
			NetMatchService service;
			service.ReportRuntimeError("resync failed: resync snapshot save failed");
			nlohmann::json report;
			try {
				report = nlohmann::json::parse(service.BuildReportJson());
			} catch (const nlohmann::json::exception& parseError) {
				*error = std::string("failed report was not JSON: ") + parseError.what();
				return false;
			}
			const nlohmann::json* admission = nullptr;
			const nlohmann::json* sessionStats = nullptr;
			if (report.contains("runner") && report["runner"].is_object() && report["runner"].contains("session") &&
			    report["runner"]["session"].is_object()) {
				const nlohmann::json& session = report["runner"]["session"];
				if (session.contains("admission") && session["admission"].is_object()) {
					admission = &session["admission"];
				}
				if (session.contains("stats") && session["stats"].is_object()) {
					sessionStats = &session["stats"];
				}
			}
			if (admission == nullptr) {
				*error = "failed report omitted runner.session.admission";
				return false;
			}
			static const char* const admissionKeys[] = {
			    "seats_dropped",
			    "applicants_registered",
			    "substitutions_committed",
			    "reclaims_accepted",
			    "incarnations_bound",
			    "substitutions_cancelled",
			    "applicants_refused",
			    "pending_applicants",
			    "substitution_offers_sent",
			    "substitution_offer_retransmits",
			    "replayed_results",
			    "seats_closed_by_leave",
			    "fenced_packets",
			    "fenced_disconnects",
			    "new_joins",
			};
			for (const char* key: admissionKeys) {
				if (!admission->contains(key) || !(*admission)[key].is_number_integer()) {
					*error = std::string("failed report missing admission.") + key;
					return false;
				}
			}
			if (sessionStats == nullptr || !sessionStats->contains("fenced_disconnects") || !sessionStats->contains("fenced_packets") ||
			    !(*sessionStats)["fenced_disconnects"].is_number_integer() || !(*sessionStats)["fenced_packets"].is_number_integer()) {
				*error = "failed report omitted session.stats fenced counters";
				return false;
			}
			return true;
		}

		bool TestRejoinWhileActivityOver(std::string* error) {
			if (NetMatchService::ResyncSnapshotAllowed(nullptr) ||
			    NetMatchService::ClassifyRejoin(nullptr) != NetRejoinAnswer::MatchOver) {
				*error = "an over or missing activity still asked for a resync snapshot";
				return false;
			}
			NetMatchService service;
			service.AnswerMatchOverRejoin("match over");
			if (service.GetState() == NetMatchServiceState::Failed) {
				*error = "a match-over rejoin failed the session";
				return false;
			}
			nlohmann::json report;
			try {
				report = nlohmann::json::parse(service.BuildReportJson());
			} catch (const nlohmann::json::exception& parseError) {
				*error = std::string("match-over report was not JSON: ") + parseError.what();
				return false;
			}
			if (!report.contains("reconnect") || report["reconnect"].value("rejoin_outcome", "") != "match_over") {
				*error = "match-over rejoin did not report rejoin_outcome=match_over";
				return false;
			}
			return true;
		}

		bool TestE2ECapUsesMatchTick(std::string* error) {
			// Returner joined at 2070: process Total() is 9935, match frame is 12005, cap is 12000.
			if (!NetMatchE2EReachedCap(9935, 12005, 12000)) {
				*error = "returner at match tick 12005 / running 9935 was not a cap completion";
				return false;
			}
			if (NetMatchE2EReachedCap(9935, 5000, 12000)) {
				*error = "returner at match tick 5000 was treated as a cap completion";
				return false;
			}
			if (!NetMatchE2EReachedCap(12001, 12005, 12000)) {
				*error = "host at match tick 12005 / running 12001 was not a cap completion";
				return false;
			}
			if (ParseLockstepStopTick("tick 12005 lockstep wait: Complete:e2e complete", 0) != 12005) {
				*error = "stop error tick 12005 was not parsed as the match frame";
				return false;
			}
			return true;
		}

		bool TestE2ETickClockCountsExecutedTicks(std::string* error) {
			NetMatchE2ETickClock heal;
			for (uint64_t tick = 4; tick <= 60; ++tick) {
				heal.NoteSimTick(tick);
			}
			heal.OnResyncRelaunch();
			for (uint64_t tick = 60; tick <= 600; ++tick) {
				heal.NoteSimTick(tick);
			}
			if (heal.Total() != 598) {
				*error = "heal clock total=" + std::to_string(heal.Total()) + " wanted 598";
				return false;
			}
			NetMatchE2ETickClock skipped;
			const uint64_t preHeal[] = {4, 30, 60};
			const uint64_t postHeal[] = {60, 300, 600};
			for (uint64_t tick: preHeal) {
				skipped.NoteSimTick(tick);
			}
			skipped.OnResyncRelaunch();
			for (uint64_t tick: postHeal) {
				skipped.NoteSimTick(tick);
			}
			if (skipped.Total() != 598) {
				*error = "skipped-note heal clock total=" + std::to_string(skipped.Total()) + " wanted 598";
				return false;
			}
			NetMatchE2ETickClock plain;
			for (uint64_t tick = 4; tick <= 604; ++tick) {
				plain.NoteSimTick(tick);
			}
			if (plain.Total() != 601) {
				*error = "plain 4..604 total=" + std::to_string(plain.Total()) + " wanted 601";
				return false;
			}
			NetMatchE2ETickClock once;
			once.NoteSimTick(10);
			once.NoteSimTick(10);
			once.NoteSimTick(10);
			if (once.Total() != 1) {
				*error = "repeated notes of one tick total=" + std::to_string(once.Total()) + " wanted 1";
				return false;
			}
			return true;
		}

		bool TestE2ETickClockSurvivesResync(std::string* error) {
			NetMatchE2ETickClock clock;
			for (uint64_t tick = 1; tick <= 250; ++tick) {
				clock.NoteSimTick(tick);
			}
			clock.OnResyncRelaunch();
			clock.NoteSimTick(1);
			if (clock.Total() < 100 || clock.EarlyOverIsSetupFailure()) {
				*error = "e2e running ticks reset at resync; total=" + std::to_string(clock.Total());
				return false;
			}
			NetMatchE2ETickClock early;
			for (uint64_t tick = 1; tick <= 20; ++tick) {
				early.NoteSimTick(tick);
			}
			if (!early.EarlyOverIsSetupFailure()) {
				*error = "an Over in the first 20 ticks was not a setup failure";
				return false;
			}
			clock.OnNewMatch();
			if (clock.Total() != 0) {
				*error = "a rematch kept the previous match's running ticks";
				return false;
			}
			clock.NoteSimTick(1);
			if (clock.Total() != 1 || !clock.EarlyOverIsSetupFailure()) {
				*error = "a rematch did not count its first executed tick";
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
			chunk.bytes.resize(NetLobbyProtocol::c_MaxStateChunkBytes);
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

		NetLobbyStateChunk MakeStateChunk(uint64_t transferId, uint32_t totalBytes, uint16_t chunkIndex, uint8_t fill = 0xA5) {
			NetLobbyStateChunk chunk;
			chunk.transferId = transferId;
			chunk.totalBytes = totalBytes;
			chunk.chunkIndex = chunkIndex;
			chunk.chunkCount = NetLobbyProtocol::GetStateChunkCount(totalBytes);
			const size_t begin = static_cast<size_t>(chunkIndex) * NetLobbyProtocol::c_MaxStateChunkBytes;
			chunk.bytes.assign(std::min(NetLobbyProtocol::c_MaxStateChunkBytes, static_cast<size_t>(totalBytes) - begin), fill);
			return chunk;
		}

		bool TestLobbyStateChunkBounds(std::string* error) {
			constexpr size_t chunkBytes = NetLobbyProtocol::c_MaxStateChunkBytes;
			constexpr uint32_t maximum = NetLobbyProtocol::c_MaxTotalStateBytes;
			if (maximum != 2147483648U || NetLobbyProtocol::GetStateChunkCount(maximum) != 43691 ||
			    NetLobbyProtocol::GetStateChunkCount(0) != 0 || NetLobbyProtocol::GetStateChunkCount(static_cast<size_t>(maximum) + 1) != 0 ||
			    NetLobbyProtocol::GetStateChunkCount(std::numeric_limits<size_t>::max()) != 0 ||
			    NetLobbyProtocol::GetStateChunkCount(2 * chunkBytes) != 2 || NetLobbyProtocol::GetStateChunkCount(2 * chunkBytes + 1) != 3) {
				*error = "state transfer bounds narrowed or overflowed";
				return false;
			}
			for (const uint32_t total: {1U, 49152U, 49153U, 67108864U, 67108865U, maximum}) {
				const uint16_t last = NetLobbyProtocol::GetStateChunkCount(total) - 1;
				if (!RoundTrip(MakeStateChunk(1, total, 0), error) || !RoundTrip(MakeStateChunk(1, total, last), error)) return false;
			}
			const auto setLE = [](std::vector<uint8_t>& bytes, size_t offset, uint64_t value, size_t count) {
				for (size_t i = 0; i < count; ++i) bytes.at(offset + i) = static_cast<uint8_t>(value >> (8 * i));
			};
			const auto reject = [&](const NetLobbyStateChunk& chunk) {
				std::vector<uint8_t> encoded;
				if (NetLobbyProtocol::Encode({chunk}, encoded)) return false;
				if (!NetLobbyProtocol::Encode({MakeStateChunk(1, 1, 0)}, encoded)) return false;
				encoded.resize(36 + chunk.bytes.size());
				setLE(encoded, 12, 20 + chunk.bytes.size(), 4);
				setLE(encoded, 16, chunk.transferId, 8);
				setLE(encoded, 24, chunk.totalBytes, 4);
				setLE(encoded, 28, chunk.chunkIndex, 2);
				setLE(encoded, 30, chunk.chunkCount, 2);
				setLE(encoded, 32, chunk.bytes.size(), 4);
				std::copy(chunk.bytes.begin(), chunk.bytes.end(), encoded.begin() + 36);
				return !NetLobbyProtocol::Decode(encoded).ok;
			};
			const NetLobbyStateChunk valid = MakeStateChunk(1, 2 * static_cast<uint32_t>(chunkBytes) + 7, 0);
			std::vector<NetLobbyStateChunk> invalid(12, valid);
			invalid[0].transferId = 0;
			invalid[1].totalBytes = 0;
			invalid[2].totalBytes = maximum + 1;
			invalid[3].chunkIndex = valid.chunkCount;
			invalid[4].chunkCount = 0;
			invalid[5].chunkCount = 65535;
			invalid[6].bytes.clear();
			invalid[7].bytes.pop_back();
			invalid[8].bytes.push_back(0);
			invalid[9].chunkIndex = valid.chunkCount - 1;
			invalid[10] = MakeStateChunk(1, 1, 0);
			invalid[10].totalBytes = maximum;
			invalid[10].chunkCount = 43691;
			invalid[11] = invalid[10];
			invalid[11].chunkCount = 1;
			for (size_t i = 0; i < invalid.size(); ++i) {
				if (!reject(invalid[i])) {
					*error = "state chunk encoder/decoder accepted malformed case " + std::to_string(i);
					return false;
				}
			}
			return true;
		}

		bool TestLobbyStateChunkConsistency(std::string* error) {
			constexpr uint32_t total = 2 * static_cast<uint32_t>(NetLobbyProtocol::c_MaxStateChunkBytes) + 7;
			const std::array<const char*, 11> cases = {"exact duplicates", "changed duplicate", "mixed total", "mixed count", "gap", "new id without first chunk", "regressed id", "unreliable chunk", "premature start", "size-only header", "maximum prefix"};
			for (size_t test = 0; test < cases.size(); ++test) {
				LoopbackTransport hostTransport, clientTransport;
				NetPeerId hostPeer = 0, clientPeer = 0;
				if (!StartLoopbackTransports(static_cast<uint16_t>(43120 + test), hostTransport, clientTransport, hostPeer, clientPeer, error)) return false;
				NetLobbySession client;
				NetLobbySessionConfig config;
				config.localPeerId = 2;
				config.remotePeerId = 1;
				config.remoteTransportPeerId = clientPeer;
				config.matchConfig = MakeConfig();
				if (!client.Start(clientTransport, config, error)) return false;
				uint64_t now = 0;
				const auto send = [&](const NetLobbyPayload& payload, NetTransportLane lane = NetTransportLane::ControlReliable) {
					std::vector<uint8_t> bytes;
					if (!NetLobbyProtocol::Encode({payload}, bytes) || !hostTransport.Send(hostPeer, lane, bytes, error)) return false;
					client.Tick(++now);
					return true;
				};
				const NetLobbyStateChunk first = MakeStateChunk(100, test == 10 ? NetLobbyProtocol::c_MaxTotalStateBytes : total, 0);
				if (!send(first) || client.IsFailed() || client.GetStateTransferProgressSerial() != 1 || client.HasCompleteStateTransfer() || !client.TakeReceivedState().empty()) {
					*error = "state transfer first chunk was not retained as partial";
					return false;
				}
				if (test == 10) {
					if (client.GetStateTransferProgress() != std::pair<uint32_t, uint32_t>{49152, NetLobbyProtocol::c_MaxTotalStateBytes}) {
						*error = "maximum state header did not retain only the received prefix";
						return false;
					}
					continue;
				}
				if (test == 0) {
					if (!send(NetLobbyPeerState{1, true, 0, 0, "Host", "test"}) || !send(first) || client.GetStateTransferProgressSerial() != 1 ||
					    !send(MakeStateChunk(100, total, 1)) || !send(first) || client.GetStateTransferProgressSerial() != 2 ||
					    !send(MakeStateChunk(100, total, 2)) || !send(MakeStateChunk(100, total, 2)) || client.IsFailed() ||
					    client.GetStateTransferProgressSerial() != 3 || !client.HasCompleteStateTransfer() ||
					    client.TakeReceivedState() != std::vector<uint8_t>(total, 0xA5) || !client.TakeReceivedState().empty()) {
						*error = "state transfer duplicates/keepalive changed progress or completed bytes";
						return false;
					}
					continue;
				}
				NetLobbyStateChunk bad = MakeStateChunk(100, total, 1);
				if (test == 1) { bad = first; bad.bytes[17] ^= 1; }
				if (test == 2) ++bad.totalBytes;
				if (test == 3) bad = MakeStateChunk(100, total + static_cast<uint32_t>(NetLobbyProtocol::c_MaxStateChunkBytes), 1);
				if (test == 4) bad = MakeStateChunk(100, total, 2);
				if (test == 5) ++bad.transferId;
				if (test == 6) {
					if (!send(MakeStateChunk(101, total, 0, 0x3C)) || client.IsFailed()) return false;
					bad = first;
				}
				const uint64_t progress = client.GetStateTransferProgressSerial();
				const auto received = client.GetStateTransferProgress();
				if (test == 8) {
					if (!send(NetLobbyStart{config.matchConfig.sessionId, 0, config.matchConfig.inputDelayFrames, client.GetMatchConfigHash()})) return false;
				} else if (test == 9) {
					std::vector<uint8_t> bytes;
					if (!NetLobbyProtocol::Encode({MakeStateChunk(100, 1, 0)}, bytes)) return false;
					for (size_t i = 0; i < 4; ++i) bytes[24 + i] = static_cast<uint8_t>(NetLobbyProtocol::c_MaxTotalStateBytes >> (8 * i));
					if (!hostTransport.Send(hostPeer, NetTransportLane::ControlReliable, bytes, error)) return false;
					client.Tick(++now);
				} else if (!send(bad, test == 7 ? NetTransportLane::InputUnreliable : NetTransportLane::ControlReliable)) return false;
				if (!client.IsFailed() || client.GetStateTransferProgressSerial() != progress || client.GetStateTransferProgress() != received) {
					*error = std::string("state transfer accepted ") + cases[test];
					return false;
				}
			}
			return true;
		}

		class StateTransferTap final: public INetTransport {
		public:
			explicit StateTransferTap(LoopbackTransport& transport): m_Transport(transport) {}
			bool StartHost(uint16_t port, std::string* error) override { return m_Transport.StartHost(port, error) && (!afterHostStart || afterHostStart(port, error)); }
			bool Connect(const std::string& address, uint16_t port, std::string* error) override { return m_Transport.Connect(address, port, error); }
			void Disconnect(NetPeerId peer, const std::string& reason) override { m_Transport.Disconnect(peer, reason); }
			void Stop() override { m_Transport.Stop(); }
			std::vector<NetTransportEvent> PollEvents() override { if (beforePoll) beforePoll(); return m_Transport.PollEvents(); }
			bool Send(NetPeerId peer, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error, bool* congested) override {
				if (!m_Transport.Send(peer, lane, bytes, error, congested)) return false;
				const auto decoded = NetLobbyProtocol::Decode(bytes);
				if (decoded.ok) {
					if (const auto* chunk = std::get_if<NetLobbyStateChunk>(&decoded.message.payload)) {
						chunks.emplace_back(peer, *chunk);
						wrongLane = wrongLane || lane != NetTransportLane::ControlReliable;
					}
					if (std::holds_alternative<NetLobbyStart>(decoded.message.payload) && afterLobbyStart) afterLobbyStart();
				}
				return true;
			}
			std::vector<std::pair<NetPeerId, NetLobbyStateChunk>> chunks;
			bool wrongLane = false;
			std::function<bool(uint16_t, std::string*)> afterHostStart;
			std::function<void()> beforePoll;
			std::function<void()> afterLobbyStart;
		private:
			LoopbackTransport& m_Transport;
		};

		bool TestLobbyStateTransferRestart(std::string* error) {
			LoopbackTransport hostTransport, clientTransport;
			NetPeerId hostPeer = 0, clientPeer = 0;
			if (!StartLoopbackTransports(43130, hostTransport, clientTransport, hostPeer, clientPeer, error)) return false;
			StateTransferTap tap(hostTransport);
			NetLobbySession host, client;
			NetLobbySessionConfig config;
			config.host = true;
			config.localPeerId = 1;
			config.remotePeerId = 2;
			config.remoteTransportPeerId = hostPeer;
			config.matchConfig = MakeConfig();
			config.autoStart = false;
			if (!host.Start(tap, config, error)) return false;
			config.host = false;
			config.localPeerId = 2;
			config.remotePeerId = 1;
			config.remoteTransportPeerId = clientPeer;
			if (!client.Start(clientTransport, config, error)) return false;
			uint64_t now = 0;
			const auto tick = [&] { host.Tick(now); client.Tick(now++); };
			const std::vector<uint8_t> original(5 * NetLobbyProtocol::c_MaxStateChunkBytes + 17, 0xA5);
			const std::vector<uint8_t> replacement(original.size(), 0x3C);
			host.BeginStateTransfer(original);
			tick();
			if (!host.HasPendingStateChunks() || client.HasCompleteStateTransfer() || client.GetStateTransferProgressSerial() != 2) {
				*error = "state transfer did not stop at its two-chunk tick budget";
				return false;
			}
			host.BeginStateTransfer(replacement);
			if (host.GetStateTransferProgressSerial() != 2) {
				*error = "restarting state transfer reset its progress serial";
				return false;
			}
			for (int i = 0; i < 8 && host.HasPendingStateChunks(); ++i) tick();
			if (host.IsFailed() || client.IsFailed() || !client.HasCompleteStateTransfer() || client.TakeReceivedState() != replacement ||
			    host.GetStateTransferProgressSerial() != 8 || client.GetStateTransferProgressSerial() != 8 || tap.chunks.size() != 8 ||
			    tap.chunks[0].second.transferId + 1 != tap.chunks[2].second.transferId || tap.chunks[2].second.chunkIndex != 0) {
				*error = "same-size restart mixed old/new state or lost progress";
				return false;
			}
			const uint64_t firstId = 0x50355354ULL ^ static_cast<uint32_t>(original.size()) ^ (6ULL << 32);
			const std::vector<uint8_t> smaller(17, 0x72);
			host.BeginStateTransfer(smaller);
			host.RequestStart();
			for (int i = 0; i < 8 && !(host.IsStarted() && client.IsStarted()); ++i) tick();
			if (!host.IsStarted() || !client.IsStarted() || client.TakeReceivedState() != smaller || tap.wrongLane ||
			    tap.chunks.size() != 9 || tap.chunks.front().second.transferId != firstId || tap.chunks.back().second.transferId != firstId + 2 ||
			    host.GetStateTransferProgressSerial() != 9 || client.GetStateTransferProgressSerial() != 9) {
				*error = "state transfer restart after consumption changed ordinary bytes or stalled start";
				return false;
			}
			if (!client.Start(clientTransport, config, error) || client.GetStateTransferProgressSerial() != 0) {
				*error = "new lobby did not reset state transfer progress";
				return false;
			}
			return true;
		}

		bool TestLobbyStateTransferBackpressure(std::string* error) {
			LoopbackTransport hostTransport, clientATransport, clientBTransport;
			if (!hostTransport.StartHost(43131, error) || !clientATransport.Connect("loopback", 43131, error) || !clientBTransport.Connect("loopback", 43131, error)) return false;
			StateTransferTap tap(hostTransport);
			NetLobbySession host, clientA, clientB;
			NetMatchConfig match = MakeConfig();
			match.peerCount = 3;
			match.players.push_back(NetMatchPlayerSlot{3, 2, false, "Client B"});
			const auto config = [&](bool isHost, uint8_t local, std::map<uint8_t, NetPeerId> peers) {
				NetLobbySessionConfig result;
				result.host = isHost;
				result.localPeerId = local;
				result.remoteTransportPeerIds = std::move(peers);
				result.matchConfig = match;
				result.autoStart = false;
				return result;
			};
			if (!host.Start(tap, config(true, 1, {{2, 1}, {3, 2}}), error) ||
			    !clientA.Start(clientATransport, config(false, 2, {{1, 1}}), error) ||
			    !clientB.Start(clientBTransport, config(false, 3, {{1, 1}}), error)) return false;
			uint64_t now = 0;
			const auto tick = [&] { host.Tick(now); clientA.Tick(now); clientB.Tick(now++); };
			for (int i = 0; i < 4; ++i) tick();
			LoopbackTransportConfig fault;
			fault.sendBufferBytes = static_cast<uint32_t>(NetLobbyProtocol::c_MaxStateChunkBytes);
			fault.meterOnlyPeer = 2;
			hostTransport.SetFaultConfig(fault);
			const std::vector<uint8_t> state(5 * NetLobbyProtocol::c_MaxStateChunkBytes + 17, 0x59);
			host.BeginStateTransfer(state);
			host.RequestStart();
			for (int i = 0; i < 4; ++i) tick();
			if (host.IsFailed() || host.IsStarted() || !host.HasPendingStateChunks() || tap.chunks.size() != 1 || tap.chunks.front().first != 1 ||
			    host.GetStateTransferProgressSerial() != 1 || clientA.GetStateTransferProgressSerial() != 1 || clientB.GetStateTransferProgressSerial() != 0) {
				*error = "state transfer retry repeated an accepted destination or started under backpressure";
				return false;
			}
			hostTransport.SetFaultConfig({});
			for (int i = 0; i < 8 && !(host.IsStarted() && clientA.IsStarted() && clientB.IsStarted()); ++i) tick();
			std::map<NetPeerId, uint16_t> nextChunk;
			for (const auto& [peer, chunk]: tap.chunks) {
				if (chunk.chunkIndex != nextChunk[peer]++ || chunk.bytes.size() > NetLobbyProtocol::c_MaxStateChunkBytes) {
					*error = "state transfer duplicated, reordered or oversized a destination chunk";
					return false;
				}
			}
			if (!host.IsStarted() || !clientA.IsStarted() || !clientB.IsStarted() || tap.wrongLane || nextChunk[1] != 6 || nextChunk[2] != 6 ||
			    clientA.TakeReceivedState() != state || clientB.TakeReceivedState() != state || host.HasPendingStateChunks() ||
			    host.GetStateTransferProgressSerial() != 12 || clientA.GetStateTransferProgressSerial() != 6 || clientB.GetStateTransferProgressSerial() != 6) {
				*error = "state transfer did not resume every destination exactly after backpressure";
				return false;
			}
			return true;
		}

		bool TestRunnerStateTransferProgress(std::string* error) {
			for (const bool stalled: {false, true}) {
				LoopbackTransport hostTransport, clientTransport;
				StateTransferTap tap(hostTransport);
				NetSession hostSession, clientSession;
				NetLobbySession clientLobby;
				NetLockstepCoordinator hostCoordinator, clientCoordinator;
				NetMatchRunner runner;
				const auto startedAt = std::chrono::steady_clock::now();
				const auto nowMs = [&] { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt).count()); };
				NetMatchRunnerConfig config;
				config.host = true;
				config.matchConfig = MakeConfig();
				config.useLobbyProtocol = true;
				config.lobbyWaitMs = 150;
				config.sessionWaitMs = 1000;
				config.lockstepWaitMs = 500;
				config.postSessionSettleMs = config.postLobbySettleMs = 0;
				config.nowMs = nowMs;
				config.sessionConfig.port = stalled ? 43134 : 43133;
				config.sessionConfig.sessionId = config.matchConfig.sessionId;
				config.sessionConfig.displayName = "Host";
				config.sessionConfig.heartbeatIntervalMs = 25;
				auto& identity = config.sessionConfig.localIdentity;
				identity.gameVersion = "7.0.0-test";
				identity.networkProtocolVersion = NetProtocol::c_Version;
				identity.controllerFrameVersion = ControllerFrame::c_Version;
				identity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
				identity.buildId = "state-transfer-selftest";
				identity.platform = "test";
				NetSessionConfig clientConfig = config.sessionConfig;
				clientConfig.displayName = "Client";
				++clientConfig.localNonce;
				tap.afterHostStart = [&](uint16_t, std::string* startError) { return clientSession.StartClient(clientTransport, "loopback", clientConfig, startError); };
				uint64_t transportClock = 0, lobbyStartedAt = 0;
				bool lobbyActive = false, coordinatorActive = false;
				std::string peerError;
				std::vector<uint8_t> received;
				// Both real loopback endpoints are pumped on the runner's thread.
				tap.beforePoll = [&] {
					const uint64_t now = nowMs();
					hostTransport.AdvanceTimeMs(now - transportClock);
					clientTransport.AdvanceTimeMs(now - transportClock);
					transportClock = now;
					if (!clientSession.IsReady()) clientSession.Tick(now);
					if (!clientSession.IsReady() || !peerError.empty()) return;
					if (!lobbyActive) {
						NetLobbySessionConfig lobbyConfig;
						lobbyConfig.localPeerId = 2;
						lobbyConfig.remotePeerId = 1;
						lobbyConfig.remoteTransportPeerId = clientSession.GetRemoteTransportPeerId();
						lobbyConfig.matchConfig = config.matchConfig;
						lobbyConfig.session = &clientSession;
						lobbyConfig.sessionNowMs = nowMs;
						lobbyActive = clientLobby.Start(clientTransport, lobbyConfig, &peerError);
						lobbyStartedAt = now;
					}
					if (!lobbyActive) return;
					if (!coordinatorActive) {
						clientLobby.Tick(now - lobbyStartedAt);
						if (!clientLobby.IsStarted()) return;
						received = clientLobby.TakeReceivedState();
						NetLockstepConfig lockstepConfig;
						lockstepConfig.sessionId = clientSession.GetSessionId();
						lockstepConfig.resumeFromSnapshot = !received.empty();
						lockstepConfig.localPeerId = 2;
						lockstepConfig.remoteTransportPeerIds = {{1, clientSession.GetRemoteTransportPeerId()}};
						lockstepConfig.matchConfig = clientLobby.GetMatchConfig();
						lockstepConfig.inputDelayFrames = NetMatchConfigUtil::PeerInputDelay(lockstepConfig.matchConfig, 2);
						lockstepConfig.startFrame = clientLobby.GetStartFrame();
						lockstepConfig.ownershipPolicy = NetMatchConfigUtil::OwnershipPolicyName(lockstepConfig.matchConfig.ownershipPolicy);
						lockstepConfig.scenario = config.scenario;
						coordinatorActive = clientCoordinator.Start(clientTransport, lockstepConfig, &peerError);
					}
					if (coordinatorActive) clientCoordinator.Tick(NetLockstepNowMs());
				};
				tap.afterLobbyStart = tap.beforePoll;
				bool transferActive = false;
				uint64_t transferStartedAt = 0, lastProgress = 0;
				std::vector<uint64_t> progressTimes;
				config.publishLobby = [&](const NetLobbySnapshot&) {
					const uint64_t progress = runner.GetLobbySession().GetStateTransferProgressSerial();
					if (transferActive && progress != lastProgress) {
						lastProgress = progress;
						progressTimes.push_back(nowMs() - transferStartedAt);
					}
				};
				if (!runner.Start(tap, hostSession, hostCoordinator, config, error) || !clientCoordinator.IsRunning()) {
					*error = "state transfer runner setup failed: " + *error + "; peer=" + peerError;
					return false;
				}
				hostCoordinator.Complete("state transfer test round");
				tap.beforePoll();
				if (!clientCoordinator.IsStopped()) { *error = "state transfer runner prior round did not stop"; return false; }
				lobbyActive = coordinatorActive = false;
				clientLobby = NetLobbySession{};
				LoopbackTransportConfig throttle;
				throttle.sendBufferBytes = static_cast<uint32_t>(NetLobbyProtocol::c_MaxStateChunkBytes) + 36 + 4096;
				throttle.drainBytesPerSecond = stalled ? 0 : 2 * 1024 * 1024;
				hostTransport.SetFaultConfig(throttle);
				const std::vector<uint8_t> state(11 * NetLobbyProtocol::c_MaxStateChunkBytes + 17, 0x67);
				const uint32_t messagesBefore = hostSession.GetStats().receivedMessages;
				transferActive = true;
				transferStartedAt = nowMs();
				std::string transferError;
				const bool ok = runner.StartNextMatch(tap, hostSession, hostCoordinator, &transferError, state);
				const uint64_t elapsed = nowMs() - transferStartedAt;
				if (progressTimes.empty() || !peerError.empty() || elapsed <= config.lobbyWaitMs || tap.wrongLane) {
					*error = "runner state transfer did not exercise its elapsed deadline; peer=" + peerError;
					return false;
				}
				if (stalled) {
					if (ok || transferError != "timed out waiting for lobby start" || lastProgress != 1 || received.size() != 0 ||
					    elapsed - progressTimes.back() <= config.lobbyWaitMs || hostSession.GetStats().receivedMessages < messagesBefore + 2) {
						*error = "runner stalled state transfer did not time out amid session keepalives: " + transferError;
						return false;
					}
				} else {
					uint64_t previous = 0;
					for (const uint64_t progressAt: progressTimes) {
						if (progressAt - previous > config.lobbyWaitMs) { *error = "throttled transfer had a real progress stall"; return false; }
						previous = progressAt;
					}
					if (!ok || received != state || lastProgress != 12 || progressTimes.back() <= config.lobbyWaitMs || !clientCoordinator.IsRunning()) {
						*error = "runner timed out a progressing state transfer: " + transferError;
						return false;
					}
				}
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

	bool TestHoldResolutionPumpDoesNotRelock(std::string* error) {
		auto pumpOne = [&](NetHoldResolution resolution) -> bool {
			NetMatchService service;
			service.m_IsHost = true;
			service.m_AdmissionAttached = true;
			service.m_State = NetMatchServiceState::Running;
			service.m_Session = std::make_unique<NetSession>();
			service.m_Coordinator = std::make_unique<NetLockstepCoordinator>();
			NetLockstepCoordinator& coordinator = *service.m_Coordinator;
			coordinator.m_RelayHost = true;
			coordinator.m_State = NetLockstepState::Running;
			coordinator.m_RemotePeerIds = {2};
			coordinator.m_DroppedSeats.insert(2);
			coordinator.m_DroppedAtMs[2] = 0;
			coordinator.m_PeerLeaveFrames[2] = 0;
			coordinator.m_LeftSeatsHeld.insert(2);
			coordinator.SetSeatStateSource(&NetMatchService::QuerySeatState, &service);
			service.m_ReconnectHost.QueueHoldResolution(2, resolution);
			service.PumpSessionEvents();
			if (coordinator.AnyDroppedSeatHeld()) {
				*error = resolution == NetHoldResolution::Expired
				             ? "Expired left the dropped seat held after PumpSessionEvents"
				             : "Reclaimed left the dropped seat held after PumpSessionEvents";
				return false;
			}
			const NetLockstepHoldResolution expected = resolution == NetHoldResolution::Expired
			                                               ? NetLockstepHoldResolution::Expired
			                                               : NetLockstepHoldResolution::Reclaimed;
			if (coordinator.HeldSeatResolution(2) != expected) {
				*error = resolution == NetHoldResolution::Expired
				             ? "Expired did not resolve the held seat"
				             : "Reclaimed did not resolve the held seat";
				return false;
			}
			return true;
		};
		return pumpOne(NetHoldResolution::Expired) && pumpOne(NetHoldResolution::Reclaimed);
	}

	bool TestRosterTransitionsRecordHoldThenPresent(std::string* error) {
		NetMatchService service;
		service.m_State = NetMatchServiceState::Running;
		NetLobbyMember host;
		host.peerId = 1;
		host.displayName = "Host";
		NetLobbyMember leaver;
		leaver.peerId = 2;
		leaver.displayName = "Leaver";
		service.m_LobbySnapshot.members = {host, leaver};
		NetLockstepSeatSnapshot snapshot;
		snapshot.senderPeerId = 1;
		snapshot.sessionId = 11;
		snapshot.roundId = 1;
		snapshot.revision = 1;
		snapshot.observedAtMs = 1000;
		NetSeatPresenceEntry seat;
		seat.stableSeat = 1;
		seat.peerId = 2;
		seat.state = NetSeatPresenceState::Reconnecting;
		seat.holdActive = true;
		seat.holdUntilMs = 21000;
		seat.holderName = "Leaver";
		snapshot.seats = {seat};
		if (!service.m_SeatPresence.ApplySnapshot(snapshot, 1000)) {
			*error = "the hold snapshot was not applied";
			return false;
		}
		service.RecordRosterTransitions(snapshot.observedAtMs);
		snapshot.revision = 2;
		snapshot.observedAtMs = 5000;
		snapshot.seats[0].state = NetSeatPresenceState::Present;
		snapshot.seats[0].holdActive = false;
		snapshot.seats[0].holdUntilMs = 0;
		if (!service.m_SeatPresence.ApplySnapshot(snapshot, 5000)) {
			*error = "the present snapshot was not applied";
			return false;
		}
		service.RecordRosterTransitions(snapshot.observedAtMs);
		nlohmann::json report;
		try {
			report = nlohmann::json::parse(service.BuildReportJson());
		} catch (const nlohmann::json::exception& parseError) {
			*error = std::string("roster-transition report was not JSON: ") + parseError.what();
			return false;
		}
		if (!report.contains("reconnect") || !report["reconnect"].contains("roster_transitions")) {
			*error = "report omitted reconnect.roster_transitions";
			return false;
		}
		const nlohmann::json& transitions = report["reconnect"]["roster_transitions"];
		if (!transitions.is_array()) {
			*error = "roster_transitions was not an array";
			return false;
		}
		std::vector<nlohmann::json> peer2;
		for (const nlohmann::json& row: transitions) {
			if (row.value("peer_id", 0) == 2) {
				peer2.push_back(row);
			}
		}
		if (peer2.size() != 2) {
			*error = "peer 2 roster_transitions size=" + std::to_string(peer2.size()) + " wanted 2";
			return false;
		}
		if (peer2[0].value("state", "") == "Present" || peer2[0].value("line", "").empty()) {
			*error = "first peer 2 transition was not a hold line";
			return false;
		}
		if (peer2[1].value("state", "") != "Present") {
			*error = "second peer 2 transition was not Present";
			return false;
		}
		const nlohmann::json& lines = report["reconnect"].value("roster_lines", nlohmann::json::array());
		if (!lines.is_array() || !lines.empty()) {
			*error = "roster_lines was not empty after the present snapshot";
			return false;
		}
		return true;
	}

	bool TestMatchOverRejoinFromWaitKeepsCoordinator(std::string* error) {
		LoopbackTransport hostTransport;
		LoopbackTransport clientTransport;
		NetPeerId hostRemotePeer = c_InvalidNetPeerId;
		NetPeerId clientRemotePeer = c_InvalidNetPeerId;
		if (!hostTransport.StartHost(43211, error) || !clientTransport.Connect("loopback", 43211, error)) {
			return false;
		}
		for (const NetTransportEvent& event: hostTransport.PollEvents()) {
			if (event.type == NetTransportEventType::PeerConnected) {
				hostRemotePeer = event.peerId;
			}
		}
		for (const NetTransportEvent& event: clientTransport.PollEvents()) {
			if (event.type == NetTransportEventType::PeerConnected) {
				clientRemotePeer = event.peerId;
			}
		}
		if (hostRemotePeer == c_InvalidNetPeerId || clientRemotePeer == c_InvalidNetPeerId) {
			*error = "match-over wait fixture did not connect the loopback pair";
			return false;
		}
		auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
			NetLockstepConfig config;
			config.sessionId = 0x4D4F5232303131ULL;
			config.timeoutMs = 2000;
			config.localPeerId = local;
			config.peerCount = 2;
			config.remoteTransportPeerIds = std::move(transports);
			config.relayToOtherPeers = relay;
			config.scenario = "LockstepSelfTest";
			config.ownershipPolicy = "unique-id-split";
			return config;
		};
		NetMatchService service;
		service.m_IsHost = true;
		service.m_State = NetMatchServiceState::Running;
		service.m_Coordinator = std::make_unique<NetLockstepCoordinator>();
		NetLockstepCoordinator client;
		if (!service.m_Coordinator->Start(hostTransport, cfg(1, {{2, hostRemotePeer}}, true), error) ||
		    !client.Start(clientTransport, cfg(2, {{1, clientRemotePeer}}, false), error)) {
			return false;
		}
		for (uint64_t now = 0; now < 80; now += 5) {
			service.m_Coordinator->Tick(now);
			client.Tick(now);
			hostTransport.AdvanceTimeMs(5);
			clientTransport.AdvanceTimeMs(5);
		}
		if (!service.m_Coordinator->IsRunning()) {
			*error = "match-over wait fixture never started the host coordinator";
			return false;
		}
		ScenarioRunner::SetLockstepCoordinator(service.m_Coordinator.get());
		bool pumped = false;
		bool coordinatorLived = false;
		ScenarioRunner::SetSessionPump([&] {
			if (pumped) {
				return;
			}
			pumped = true;
			service.AnswerMatchOverRejoin("match over");
			coordinatorLived = ScenarioRunner::HasLockstepCoordinator();
		});
		NetLockstepReadyFrame ready;
		std::string waitError;
		const uint64_t waitTick = service.m_Coordinator->GetStats().nextFrame;
		const bool got = ScenarioRunner::WaitForLockstepControllerFrame(waitTick, ready, &waitError);
		const bool stillThere = ScenarioRunner::HasLockstepCoordinator();
		ScenarioRunner::SetSessionPump(nullptr);
		ScenarioRunner::SetLockstepCoordinator(nullptr);
		if (!pumped) {
			*error = "the lockstep wait never ran the session pump";
			return false;
		}
		if (!coordinatorLived || !stillThere) {
			*error = "match-over rejoin from the session pump destroyed the coordinator";
			return false;
		}
		if (got) {
			*error = "the wait produced a frame after a match-over rejoin";
			return false;
		}
		if (waitError.find("Complete:") == std::string::npos || waitError.find("match over") == std::string::npos) {
			*error = "the wait did not return Complete:match over; got " + waitError;
			return false;
		}
		if (service.GetState() != NetMatchServiceState::Running) {
			*error = "the pump completed or failed the service; the main loop must FinishMatch";
			return false;
		}
		nlohmann::json report;
		try {
			report = nlohmann::json::parse(service.BuildReportJson());
		} catch (const nlohmann::json::exception& parseError) {
			*error = std::string("match-over wait report was not JSON: ") + parseError.what();
			return false;
		}
		if (!report.contains("reconnect") || report["reconnect"].value("rejoin_outcome", "") != "match_over") {
			*error = "match-over rejoin from the wait did not report rejoin_outcome=match_over";
			return false;
		}
		service.FinishMatch("match over");
		if (service.GetState() != NetMatchServiceState::Completed) {
			*error = "the main-loop FinishMatch did not complete the match";
			return false;
		}
		return true;
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
		if (!TestLobbyStateChunkBounds(&error)) return fail(error);
		if (!TestLobbyStateChunkConsistency(&error)) return fail(error);
		if (!TestLobbyStateTransferRestart(&error)) return fail(error);
		if (!TestLobbyStateTransferBackpressure(&error)) return fail(error);
		if (!TestRunnerStateTransferProgress(&error)) return fail(error);
		if (!TestLobbyThreePeer(&error)) return fail(error);
		if (!TestLobbyLateJoinerRosterRace(&error)) return fail(error);
		if (!TestServiceRuntimeErrorSurface(&error)) return fail(error);
		std::string failedReportError;
		std::string rejoinOverError;
		std::string rejoinWaitError;
		std::string tickClockError;
		std::string capTickError;
		std::string executedTickError;
		if (!TestFailedReportKeepsAdmissionCounters(&failedReportError)) {
			std::cerr << "[net-match-selftest] FAIL: " << failedReportError << std::endl;
		}
		if (!TestRejoinWhileActivityOver(&rejoinOverError)) {
			std::cerr << "[net-match-selftest] FAIL: " << rejoinOverError << std::endl;
		}
		if (!TestMatchOverRejoinFromWaitKeepsCoordinator(&rejoinWaitError)) {
			std::cerr << "[net-match-selftest] FAIL: " << rejoinWaitError << std::endl;
		}
		if (!TestE2ETickClockSurvivesResync(&tickClockError)) {
			std::cerr << "[net-match-selftest] FAIL: " << tickClockError << std::endl;
		}
		if (!TestE2ECapUsesMatchTick(&capTickError)) {
			std::cerr << "[net-match-selftest] FAIL: " << capTickError << std::endl;
		}
		if (!TestE2ETickClockCountsExecutedTicks(&executedTickError)) {
			std::cerr << "[net-match-selftest] FAIL: " << executedTickError << std::endl;
		}
		if (!failedReportError.empty()) return fail(failedReportError);
		if (!rejoinOverError.empty()) return fail(rejoinOverError);
		if (!rejoinWaitError.empty()) return fail(rejoinWaitError);
		if (!tickClockError.empty()) return fail(tickClockError);
		if (!capTickError.empty()) return fail(capTickError);
		if (!executedTickError.empty()) return fail(executedTickError);
		if (!TestHoldResolutionPumpDoesNotRelock(&error)) return fail(error);
		if (!TestRosterTransitionsRecordHoldThenPresent(&error)) return fail(error);

		std::cout << "[net-match-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
