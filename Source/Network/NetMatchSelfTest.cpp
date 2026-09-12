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
#include "ActivityMan.h"
#include "ScenarioRunner.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
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

		bool TestMatchConfigDedicated(std::string* error) {
			NetMatchConfig dedicated = MakeConfig();
			dedicated.peerCount = 3;
			dedicated.dedicated = true;
			dedicated.players = {
			    NetMatchPlayerSlot{2, 0, false, "Client A"},
			    NetMatchPlayerSlot{3, 1, false, "Client B"},
			};
			if (!NetMatchConfigUtil::ValidateLocalAlpha(dedicated, error)) {
				return false;
			}
			std::string validationError;
			NetMatchConfig seatless = dedicated;
			seatless.dedicated = false;
			if (NetMatchConfigUtil::ValidateLocalAlpha(seatless, &validationError) ||
			    validationError != "host player slot is missing") {
				*error = "seatless roster without the flag did not fail closed: " + validationError;
				return false;
			}
			NetMatchConfig hostSeated = dedicated;
			hostSeated.players.push_back(NetMatchPlayerSlot{1, 2, false, "Host"});
			validationError.clear();
			if (NetMatchConfigUtil::ValidateLocalAlpha(hostSeated, &validationError) ||
			    validationError != "dedicated config must not seat the host peer") {
				*error = "dedicated roster with a host slot did not fail closed: " + validationError;
				return false;
			}
			NetMatchConfig noClients = dedicated;
			noClients.players = {NetMatchPlayerSlot{0, 3, true, "CPU"}};
			validationError.clear();
			if (NetMatchConfigUtil::ValidateLocalAlpha(noClients, &validationError) ||
			    validationError != "dedicated config has no client player slot") {
				*error = "dedicated roster without a client slot did not fail closed: " + validationError;
				return false;
			}
			NetMatchConfig plain = MakeConfig();
			const NetHash32 plainHash = NetMatchConfigUtil::HashConfig(plain);
			if (NetMatchConfigUtil::HashConfig(plain) != plainHash) {
				*error = "match config hash is not stable";
				return false;
			}
			NetMatchConfig flagged = plain;
			flagged.dedicated = true;
			if (NetMatchConfigUtil::HashConfig(flagged) == plainHash) {
				*error = "the dedicated flag did not change the match config hash";
				return false;
			}
			flagged.dedicated = false;
			if (NetMatchConfigUtil::HashConfig(flagged) != plainHash) {
				*error = "clearing the dedicated flag changed the match config hash";
				return false;
			}
			const std::string reportJson = NetMatchConfigUtil::BuildReportJson(dedicated);
			if (reportJson.find("\"dedicated\":true") == std::string::npos) {
				*error = "the config report omitted dedicated=true";
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

		bool TestLobbyCodecDedicatedFlag(std::string* error) {
			NetMatchConfig dedicated = MakeConfig();
			dedicated.peerCount = 3;
			dedicated.dedicated = true;
			dedicated.players = {
			    NetMatchPlayerSlot{2, 0, false, "Client A"},
			    NetMatchPlayerSlot{3, 1, false, "Client B"},
			};
			if (!RoundTrip(NetLobbyMatchConfig{dedicated}, error)) {
				return false;
			}
			// The config's reserved u16 sits 16 bytes into the payload (after mode/ownership).
			const size_t reservedOffset = NetLobbyProtocol::c_HeaderBytes + 16;
			NetLobbyMessage message;
			message.payload = NetLobbyMatchConfig{dedicated};
			std::vector<uint8_t> bytes;
			NetLobbyError encodeError;
			if (!NetLobbyProtocol::Encode(message, bytes, &encodeError) ||
			    bytes.size() <= reservedOffset + 1 || bytes[reservedOffset] != 1 || bytes[reservedOffset + 1] != 0) {
				*error = "dedicated config did not encode reserved bit 0";
				return false;
			}
			const NetLobbyDecodeResult dedicatedDecoded = NetLobbyProtocol::Decode(bytes);
			const NetLobbyMatchConfig* dedicatedConfig = dedicatedDecoded.ok ? std::get_if<NetLobbyMatchConfig>(&dedicatedDecoded.message.payload) : nullptr;
			if (!dedicatedConfig || !dedicatedConfig->config.dedicated) {
				*error = "a reserved word of 1 did not decode to dedicated=true";
				return false;
			}
			message.payload = NetLobbyMatchConfig{MakeConfig()};
			if (!NetLobbyProtocol::Encode(message, bytes, &encodeError)) {
				*error = "could not encode a non-dedicated config";
				return false;
			}
			if (bytes[reservedOffset] != 0 || bytes[reservedOffset + 1] != 0) {
				*error = "non-dedicated config wrote a nonzero reserved word";
				return false;
			}
			const NetLobbyDecodeResult plainDecoded = NetLobbyProtocol::Decode(bytes);
			const NetLobbyMatchConfig* plainConfig = plainDecoded.ok ? std::get_if<NetLobbyMatchConfig>(&plainDecoded.message.payload) : nullptr;
			if (!plainConfig || plainConfig->config.dedicated) {
				*error = "a reserved word of 0 did not decode to dedicated=false";
				return false;
			}
			bytes[reservedOffset] = 2;
			const NetLobbyDecodeResult refused = NetLobbyProtocol::Decode(bytes);
			if (refused.ok || refused.error.code != NetLobbyErrorCode::ReservedFieldNonZero) {
				*error = "a reserved word of 2 was not refused";
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

		bool TestJoinWaitTrigger(std::string* error) {
			std::error_code fsError;
			const std::filesystem::path dir = std::filesystem::temp_directory_path() / "cccp-join-wait-selftest";
			std::filesystem::create_directories(dir, fsError);
			const std::filesystem::path missing = dir / "absent.trigger";
			std::filesystem::remove(missing, fsError);

			std::string waitError;
			auto started = std::chrono::steady_clock::now();
			if (NetMatchService::WaitForJoinTrigger(missing.string(), 300, &waitError)) {
				*error = "the join wait proceeded with no trigger file";
				return false;
			}
			auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
			if (waitError.find("join wait timed out") == std::string::npos || waitError.find(missing.string()) == std::string::npos) {
				*error = "the join wait timeout did not name itself and the path: " + waitError;
				return false;
			}
			if (elapsedMs < 300) {
				*error = "the join wait gave up before its budget: " + std::to_string(elapsedMs) + "ms";
				return false;
			}

			const std::filesystem::path present = dir / "present.trigger";
			{
				std::ofstream handle(present, std::ios::binary);
				handle << "go";
			}
			waitError.clear();
			if (!NetMatchService::WaitForJoinTrigger(present.string(), 300, &waitError) || !waitError.empty()) {
				*error = "the join wait did not proceed with the trigger present: " + waitError;
				return false;
			}

			const std::filesystem::path late = dir / "late.trigger";
			std::filesystem::remove(late, fsError);
			std::thread writer([late] {
				std::this_thread::sleep_for(std::chrono::milliseconds(400));
				std::ofstream handle(late, std::ios::binary);
				handle << "go";
			});
			started = std::chrono::steady_clock::now();
			const bool released = NetMatchService::WaitForJoinTrigger(late.string(), 10000, &waitError);
			elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
			writer.join();
			std::filesystem::remove(late, fsError);
			std::filesystem::remove(present, fsError);
			if (!released) {
				*error = "the join wait missed a trigger created while it waited: " + waitError;
				return false;
			}
			if (elapsedMs < 400 || elapsedMs > 4000) {
				*error = "the join wait did not release on the trigger: " + std::to_string(elapsedMs) + "ms";
				return false;
			}
			return true;
		}

		bool TestResyncReportAbsentWhenIdle(std::string* error) {
			NetMatchService service;
			nlohmann::json report;
			try {
				report = nlohmann::json::parse(service.BuildReportJson());
			} catch (const nlohmann::json::exception& parseError) {
				*error = std::string("idle report was not JSON: ") + parseError.what();
				return false;
			}
			if (report.contains("resync")) {
				*error = "idle service report carried a resync block";
				return false;
			}
			return true;
		}

		bool TestSaveCompressionChoice(std::string* error) {
			if (ActivityMan::ZipLevelFor(ActivityMan::SaveCompression::Fast) != ActivityMan::c_SaveZipLevelFast) {
				*error = "Fast save compression is not the user-save zip level";
				return false;
			}
			if (ActivityMan::ZipLevelFor(ActivityMan::SaveCompression::Small) != ActivityMan::c_SaveZipLevelSmall) {
				*error = "Small save compression is not the resync zip level";
				return false;
			}
			if (ActivityMan::c_SaveZipLevelSmall <= ActivityMan::c_SaveZipLevelFast) {
				*error = "Small must deflate harder than Fast";
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

		bool TestHealedRoundPlannedEndIsNotAFailure(std::string* error) {
			constexpr uint64_t c_Cap = 600;
			constexpr uint64_t c_Resume = 61; // The frame both peers' healed round resumes at.
			constexpr uint64_t c_StopFrame = 599; // The frame the sighted rounds stopped on.

			std::vector<std::string> failures;
			// The sighted host: its own clock 597 against the round's frame 599 and a 600-frame plan.
			if (!NetMatchE2ERoundReachedPlannedEnd(c_NetMatchE2ECompleteStop, 597, c_StopFrame, c_Cap)) {
				failures.push_back("a peer at frame " + std::to_string(c_StopFrame) + " read the round's completion as a failure");
			}
			// A break is still a break: only the completion of the planned round rides through.
			if (NetMatchE2ERoundReachedPlannedEnd("MissingFrameTimeout:peer 2", 597, c_StopFrame, c_Cap)) {
				failures.push_back("a peer lost before the planned end was read as a completion");
			}
			if (NetMatchE2ERoundReachedPlannedEnd("Desync:sim state diverged at tick 60 (Client)", 601, 601, c_Cap)) {
				failures.push_back("a desync was read as a completion");
			}

			// The client kept ticking to 62 while the host's round stopped at 58, so the relaunch
			// replays 61 and 62 for one peer and skips 59 and 60 for the other.
			NetMatchE2ETickClock client;
			for (uint64_t tick = 1; tick <= 62; ++tick) {
				client.NoteSimTick(tick);
			}
			client.OnResyncRelaunch(c_Resume);
			NetMatchE2ETickClock host;
			for (uint64_t tick = 1; tick <= 58; ++tick) {
				host.NoteSimTick(tick);
			}
			host.OnResyncRelaunch(c_Resume);
			uint64_t hostStop = 0;
			uint64_t clientStop = 0;
			uint64_t hostAtStopFrame = 0;
			uint64_t clientAtStopFrame = 0;
			for (uint64_t tick = c_Resume; tick <= c_Cap + 8; ++tick) {
				host.NoteSimTick(tick);
				client.NoteSimTick(tick);
				if (tick == c_StopFrame) {
					hostAtStopFrame = host.Total();
					clientAtStopFrame = client.Total();
				}
				if (hostStop == 0 && host.Total() > c_Cap) {
					hostStop = tick;
				}
				if (clientStop == 0 && client.Total() > c_Cap) {
					clientStop = tick;
				}
			}
			if (hostAtStopFrame != c_StopFrame || clientAtStopFrame != c_StopFrame) {
				failures.push_back("healed round clock disagrees with its frame " + std::to_string(c_StopFrame) +
				                   ": host=" + std::to_string(hostAtStopFrame) + " client=" + std::to_string(clientAtStopFrame));
			}
			if (hostStop != c_Cap + 1 || clientStop != c_Cap + 1) {
				failures.push_back("healed round ended off its planned frame " + std::to_string(c_Cap + 1) +
				                   ": host=" + std::to_string(hostStop) + " client=" + std::to_string(clientStop));
			}

			// A peer whose first observed tick after the relaunch sits below the resume frame must not
			// count the replayed frames a second time.
			NetMatchE2ETickClock behind;
			for (uint64_t tick = 1; tick <= 58; ++tick) {
				behind.NoteSimTick(tick);
			}
			behind.OnResyncRelaunch(c_Resume);
			behind.NoteSimTick(c_Resume - 2);
			const uint64_t behindBeforeResume = behind.Total();
			if (behindBeforeResume != c_Resume - 1) {
				failures.push_back("a peer first seen at frame " + std::to_string(c_Resume - 2) + " after the relaunch read total " +
				                   std::to_string(behindBeforeResume) + " instead of " + std::to_string(c_Resume - 1));
			}
			for (uint64_t tick = c_Resume; tick <= c_StopFrame; ++tick) {
				behind.NoteSimTick(tick);
			}
			const uint64_t behindAtStopFrame = behind.Total();
			if (behindAtStopFrame != c_StopFrame) {
				failures.push_back("a peer first seen below the resume frame read total " + std::to_string(behindAtStopFrame) +
				                   " at frame " + std::to_string(c_StopFrame));
			}
			if (!failures.empty()) {
				*error = "healed round planned end: " + std::to_string(failures.size()) + " defects";
				for (const std::string& failure: failures) {
					*error += "; " + failure;
				}
				return false;
			}
			std::cout << "[net-match-selftest] healed round planned end: host_total=" << hostAtStopFrame
			          << " client_total=" << clientAtStopFrame << " behind_total=" << behindAtStopFrame
			          << " at frame " << c_StopFrame << ", both stop at " << hostStop << std::endl;
			return true;
		}

		bool TestEarlyOverUsesMatchTick(std::string* error) {
			NetMatchE2ETickClock lateJoin;
			for (uint64_t tick = 1; tick <= 65; ++tick) {
				lateJoin.NoteSimTick(tick);
			}
			if (lateJoin.EarlyOverIsSetupFailure(307)) {
				*error = "own count 65 with match tick 307 was a setup failure";
				return false;
			}
			if (!lateJoin.EarlyOverIsSetupFailure(65)) {
				*error = "own count 65 with match tick 65 was not a setup failure";
				return false;
			}
			NetMatchE2ETickClock longRun;
			for (uint64_t tick = 1; tick <= 3000; ++tick) {
				longRun.NoteSimTick(tick);
			}
			if (!longRun.EarlyOverIsSetupFailure(50)) {
				*error = "own count 3000 with match tick 50 was not a setup failure";
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

		bool TestRematchRosterDerivation(std::string* error) {
			NetMatchConfig four = MakeConfig();
			four.peerCount = 4;
			four.inputDelayFrames = 1;
			four.peerInputDelayFrames = {1, 2, 3, 4};
			four.players = {
			    NetMatchPlayerSlot{1, 0, false, "Host"},
			    NetMatchPlayerSlot{2, 1, false, "Client 2"},
			    NetMatchPlayerSlot{3, 2, false, "Client 3"},
			    NetMatchPlayerSlot{4, 3, false, "Client 4"},
			};
			NetMatchConfig derived;
			std::map<uint8_t, uint8_t> seatMap;
			if (!NetMatchConfigUtil::DeriveRematchConfig(four, {1, 2, 4}, derived, &seatMap, error)) {
				return false;
			}
			if (derived.peerCount != 3 || derived.hostPeerId != 1 || seatMap != std::map<uint8_t, uint8_t>{{1, 1}, {2, 2}, {4, 3}}) {
				*error = "the rematch roster did not close up onto 1..3";
				return false;
			}
			if (derived.players.size() != 3 || derived.players[0].peerId != 1 || derived.players[1].peerId != 2 ||
			    derived.players[2].peerId != 3 || derived.players[2].displayName != "Client 4" || derived.players[2].team != 3) {
				*error = "the rematch roster did not keep the survivors' seats in order";
				return false;
			}
			if (std::any_of(derived.players.begin(), derived.players.end(), [](const NetMatchPlayerSlot& slot) { return slot.displayName == "Client 3"; })) {
				*error = "the leaver kept a seat in the derived roster";
				return false;
			}
			if (derived.peerInputDelayFrames != std::vector<uint16_t>{1, 2, 4}) {
				*error = "per-peer input delays did not follow the survivors";
				return false;
			}
			// The proposal a client accepts is the roster it derived; the round it just played is not.
			if (!NetMatchRunner::RematchRostersAgree(derived, derived) || NetMatchRunner::RematchRostersAgree(four, derived)) {
				*error = "the rematch proposal check accepted a roster the peer did not derive";
				return false;
			}
			NetMatchConfig intact;
			if (!NetMatchConfigUtil::DeriveRematchConfig(four, {1, 2, 3, 4}, intact, nullptr, error)) {
				return false;
			}
			if (NetMatchConfigUtil::HashConfig(intact) != NetMatchConfigUtil::HashConfig(four) || !NetMatchRunner::RematchRostersAgree(intact, four)) {
				*error = "an intact roster did not derive the config it played";
				return false;
			}
			std::string refusal;
			if (NetMatchConfigUtil::DeriveRematchConfig(four, {2, 4}, derived, nullptr, &refusal) || refusal.find("host") == std::string::npos) {
				*error = "a roster without the host was derived: " + refusal;
				return false;
			}
			if (NetMatchConfigUtil::DeriveRematchConfig(four, {1, 5}, derived, nullptr, &refusal) || refusal.find("outside") == std::string::npos) {
				*error = "a roster naming an unseated peer was derived: " + refusal;
				return false;
			}
			return true;
		}

		// A rematch after a peer leaves re-forms the round on the peers still connected: the match config
		// drops to the survivor count and their lockstep ids close up, or the relaunch cannot start.
		bool TestRematchRebuildsTheSurvivingRoster(std::string* error) {
			struct ClientPeer {
				LoopbackTransport transport;
				NetSession session;
				NetLobbySession lobby;
				NetLockstepCoordinator coordinator;
				uint8_t peerId = 0;
				bool lobbyActive = false;
				bool coordinatorActive = false;
				bool left = false;
				uint64_t lobbyStartedAt = 0;
			};
			std::array<ClientPeer, 3> clients;
			LoopbackTransport hostTransport;
			StateTransferTap tap(hostTransport);
			NetSession hostSession;
			NetLockstepCoordinator round1, round2;
			NetMatchRunner runner;
			const auto startedAt = std::chrono::steady_clock::now();
			const auto nowMs = [&] { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt).count()); };

			NetMatchRunnerConfig config;
			config.host = true;
			config.matchConfig = MakeConfig();
			config.matchConfig.peerCount = 4;
			config.matchConfig.players = {
			    NetMatchPlayerSlot{1, 0, false, "Host"},
			    NetMatchPlayerSlot{2, 1, false, "Client 2"},
			    NetMatchPlayerSlot{3, 2, false, "Client 3"},
			    NetMatchPlayerSlot{4, 3, false, "Client 4"},
			};
			config.useLobbyProtocol = true;
			config.sessionWaitMs = 8000;
			config.lobbyWaitMs = 8000;
			config.lockstepWaitMs = 4000;
			config.missingFrameGraceMs = 30000;
			config.postSessionSettleMs = config.postLobbySettleMs = 0;
			config.startFrame = 1;
			config.scenario = "rematch-roster-selftest";
			config.nowMs = nowMs;
			config.sessionConfig.port = 43140;
			config.sessionConfig.maxPeers = 3;
			config.sessionConfig.timeoutMs = 30000;
			config.sessionConfig.heartbeatIntervalMs = 25;
			config.sessionConfig.sessionId = config.matchConfig.sessionId;
			config.sessionConfig.displayName = "Host";
			auto& identity = config.sessionConfig.localIdentity;
			identity.gameVersion = "7.0.0-test";
			identity.networkProtocolVersion = NetProtocol::c_Version;
			identity.controllerFrameVersion = ControllerFrame::c_Version;
			identity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
			identity.buildId = "rematch-roster-selftest";
			identity.platform = "test";

			std::string peerError;
			uint64_t transportClock = 0;
			NetMatchConfig clientRoster = config.matchConfig;
			auto advance = [&] {
				const uint64_t now = nowMs();
				const uint64_t delta = now >= transportClock ? now - transportClock : 0;
				hostTransport.AdvanceTimeMs(delta);
				for (ClientPeer& client: clients) client.transport.AdvanceTimeMs(delta);
				transportClock = now;
			};
			auto pumpClient = [&](ClientPeer& client) {
				const uint64_t now = nowMs();
				if (client.left || !peerError.empty()) return;
				if (!client.session.IsReady()) {
					client.session.Tick(now);
					return;
				}
				if (client.peerId == 0) client.peerId = static_cast<uint8_t>(client.session.GetLocalPeerId() + 1);
				if (!client.lobbyActive) {
					NetLobbySessionConfig lobbyConfig;
					lobbyConfig.localPeerId = client.peerId;
					lobbyConfig.remotePeerId = config.matchConfig.hostPeerId;
					lobbyConfig.remoteTransportPeerId = client.session.GetRemoteTransportPeerId();
					lobbyConfig.matchConfig = clientRoster;
					lobbyConfig.session = &client.session;
					lobbyConfig.sessionNowMs = nowMs;
					lobbyConfig.timeoutMs = 30000;
					lobbyConfig.displayName = "Client " + std::to_string(client.peerId);
					client.lobbyActive = client.lobby.Start(client.transport, lobbyConfig, &peerError);
					client.lobbyStartedAt = now;
				}
				if (!client.lobbyActive) return;
				if (!client.coordinatorActive) {
					client.lobby.Tick(now - client.lobbyStartedAt);
					if (!client.lobby.IsStarted()) return;
					NetLockstepConfig lockstepConfig;
					lockstepConfig.sessionId = client.session.GetSessionId();
					lockstepConfig.localPeerId = client.peerId;
					lockstepConfig.peerCount = client.lobby.GetMatchConfig().peerCount;
					lockstepConfig.remoteTransportPeerIds = {{config.matchConfig.hostPeerId, client.session.GetRemoteTransportPeerId()}};
					lockstepConfig.matchConfig = client.lobby.GetMatchConfig();
					lockstepConfig.inputDelayFrames = NetMatchConfigUtil::PeerInputDelay(lockstepConfig.matchConfig, client.peerId);
					lockstepConfig.startFrame = client.lobby.GetStartFrame();
					lockstepConfig.ownershipPolicy = NetMatchConfigUtil::OwnershipPolicyName(lockstepConfig.matchConfig.ownershipPolicy);
					lockstepConfig.scenario = config.scenario;
					lockstepConfig.timeoutMs = 30000;
					client.coordinatorActive = client.coordinator.Start(client.transport, lockstepConfig, &peerError);
				}
				if (client.coordinatorActive) client.coordinator.Tick(NetLockstepNowMs());
			};
			auto pumpClients = [&] {
				advance();
				for (ClientPeer& client: clients) pumpClient(client);
			};
			tap.beforePoll = pumpClients;
			tap.afterLobbyStart = pumpClients;
			tap.afterHostStart = [&](uint16_t, std::string* startError) {
				for (ClientPeer& client: clients) {
					NetSessionConfig clientConfig = config.sessionConfig;
					clientConfig.displayName = "Client";
					clientConfig.localNonce += 1 + static_cast<uint64_t>(&client - clients.data());
					if (!client.session.StartClient(client.transport, "loopback", clientConfig, startError)) return false;
				}
				return true;
			};

			if (!runner.Start(tap, hostSession, round1, config, error)) {
				*error = "four-peer round 1 setup failed: " + *error + "; peer=" + peerError;
				return false;
			}
			for (int spin = 0; spin < 400 && (!clients[0].coordinator.IsRunning() || !clients[1].coordinator.IsRunning() || !clients[2].coordinator.IsRunning()); ++spin) {
				pumpClients();
				round1.Tick(NetLockstepNowMs());
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			if (!clients[0].coordinator.IsRunning() || !clients[1].coordinator.IsRunning() || !clients[2].coordinator.IsRunning()) {
				*error = "four-peer round 1 did not reach Running on every client; peer=" + peerError;
				return false;
			}
			// The middle seat leaves, so the survivors' ids are sparse (1, 2, 4) and renumbering is the
			// only way to a dense 1..3 roster.
			ClientPeer* leaver = nullptr;
			for (ClientPeer& client: clients) {
				if (client.peerId == 3) leaver = &client;
			}
			if (!leaver) {
				*error = "no client took lockstep peer 3";
				return false;
			}
			const uint8_t leaverPeerId = leaver->peerId;
			leaver->coordinator.Leave("player left");
			for (int spin = 0; spin < 400 && !round1.GetPeerLeaveFrames().contains(leaverPeerId); ++spin) {
				pumpClients();
				round1.Tick(NetLockstepNowMs());
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			if (!round1.GetPeerLeaveFrames().contains(leaverPeerId)) {
				*error = "the host round never saw the clean leave";
				return false;
			}
			leaver->left = true;
			leaver->transport.Stop();
			round1.Complete("round over");
			for (int spin = 0; spin < 400 && hostSession.GetReadyPeerCount() != 2; ++spin) {
				advance();
				hostSession.Tick(nowMs());
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			if (hostSession.GetReadyPeerCount() != 2) {
				*error = "the host session still holds the leaver: ready=" + std::to_string(hostSession.GetReadyPeerCount());
				return false;
			}

			// The survivors' own derivation of the rematch roster: the round-1 ids that did not leave,
			// in their old order, renumbered 1..N.
			std::vector<uint8_t> survivors{config.matchConfig.hostPeerId};
			for (ClientPeer& client: clients) {
				if (!client.left) survivors.push_back(client.peerId);
			}
			std::sort(survivors.begin(), survivors.end());
			clientRoster = config.matchConfig;
			clientRoster.peerCount = static_cast<uint8_t>(survivors.size());
			clientRoster.players.clear();
			for (size_t index = 0; index < survivors.size(); ++index) {
				for (const NetMatchPlayerSlot& slot: config.matchConfig.players) {
					if (slot.peerId != survivors[index]) continue;
					NetMatchPlayerSlot moved = slot;
					moved.peerId = static_cast<uint8_t>(index + 1);
					clientRoster.players.push_back(moved);
				}
			}
			for (ClientPeer& client: clients) {
				if (client.left) continue;
				const auto found = std::find(survivors.begin(), survivors.end(), client.peerId);
				client.peerId = static_cast<uint8_t>(std::distance(survivors.begin(), found) + 1);
				client.lobbyActive = client.coordinatorActive = false;
				client.lobby = NetLobbySession{};
			}
			runner.SetStartFrame(1);
			std::string rematchError;
			const bool relaunched = runner.StartNextMatch(tap, hostSession, round2, &rematchError);
			if (!relaunched) {
				*error = "rematch relaunch failed: " + rematchError + "; peer=" + peerError;
				return false;
			}
			const NetMatchConfig& rematchConfig = runner.GetMatchConfig();
			if (rematchConfig.peerCount != 3) {
				*error = "rematch config kept peer_count " + std::to_string(rematchConfig.peerCount);
				return false;
			}
			std::vector<uint8_t> seated;
			for (const NetMatchPlayerSlot& slot: rematchConfig.players) {
				if (!slot.cpu) seated.push_back(slot.peerId);
				if (slot.displayName == "Client 3") {
					*error = "the leaver kept a seat in the rematch roster";
					return false;
				}
			}
			std::sort(seated.begin(), seated.end());
			if (seated != std::vector<uint8_t>{1, 2, 3}) {
				*error = "rematch roster ids are not dense 1..3";
				return false;
			}
			for (ClientPeer& client: clients) {
				if (client.left) continue;
				for (int spin = 0; spin < 400 && !client.coordinator.IsRunning(); ++spin) {
					pumpClients();
					round2.Tick(NetLockstepNowMs());
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				if (!client.coordinator.IsRunning() || client.coordinator.GetConfig().peerCount != 3) {
					*error = "a survivor did not reach Running on the rematch roster; peer=" + peerError;
					return false;
				}
			}
			round2.Complete("rematch over");
			for (ClientPeer& client: clients) client.transport.Stop();
			hostTransport.Stop();
			return true;
		}

		// Loopback transports hand packets into each other's queues, so all calls share one lock.
		class LockedLoopback final : public INetTransport {
		public:
			~LockedLoopback() override {
				std::lock_guard<std::mutex> lock(Mutex());
				m_Transport.Stop();
			}

			bool StartHost(uint16_t port, std::string* error) override {
				std::lock_guard<std::mutex> lock(Mutex());
				m_Hosting = m_Transport.StartHost(port, error);
				return m_Hosting;
			}
			bool Connect(const std::string& address, uint16_t port, std::string* error) override {
				std::lock_guard<std::mutex> lock(Mutex());
				return m_Transport.Connect(address, port, error);
			}
			bool Send(NetPeerId peerId, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error, bool* congested) override {
				std::lock_guard<std::mutex> lock(Mutex());
				return m_Transport.Send(peerId, lane, bytes, error, congested);
			}
			void Disconnect(NetPeerId peerId, const std::string& reason) override {
				std::lock_guard<std::mutex> lock(Mutex());
				m_Transport.Disconnect(peerId, reason);
			}
			void Stop() override {
				std::lock_guard<std::mutex> lock(Mutex());
				m_Transport.Stop();
				m_Hosting = false;
			}
			std::vector<NetTransportEvent> PollEvents() override {
				std::lock_guard<std::mutex> lock(Mutex());
				return m_Transport.PollEvents();
			}
			bool IsHosting() const {
				std::lock_guard<std::mutex> lock(Mutex());
				return m_Hosting;
			}

		private:
			static std::mutex& Mutex() {
				static std::mutex mutex;
				return mutex;
			}

			LoopbackTransport m_Transport;
			bool m_Hosting = false;
		};

		// Real elapsed time plus what a test skips, so a reclaim window can lapse without sleeping.
		struct RematchClock {
			const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
			std::atomic<uint64_t> skippedMs{0};

			uint64_t NowMs() const {
				return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - origin).count()) + skippedMs.load();
			}
		};

		// One peer's match objects, pumped the way NetMatchService and the sim thread pump them.
		struct RematchPeer {
			std::string name;
			bool host = false;
			bool leaving = false; //!< Its §7 exchange owns the session; the sim no longer ticks its round.
			bool gone = false;
			LockedLoopback transport;
			NetSession session;
			NetMatchRunner runner;
			NetMatchRunnerConfig config;
			std::unique_ptr<NetLockstepCoordinator> round;
			NetSeatAuthRegistry registry;
			NetReconnectHost admission;
			NetReconnectTicketStore store;
			NetReconnectClient reconnect;
			std::vector<NetTransportEvent> handover;      //!< What the round handed the session, drained like PumpSessionEvents.
			std::optional<NetLockstepSeatSnapshot> seats; //!< Client: the host's latest seat view this round.
			std::map<uint64_t, std::string> trace;        //!< Every tick this round committed, as the sim applied it.
			uint64_t nextProduce = 0;
			uint64_t lastApplied = 0;

			uint8_t LockstepId() const { return static_cast<uint8_t>(session.GetLocalPeerId() + 1); }
		};

		struct RematchFixture {
			RematchClock clock;
			std::vector<std::unique_ptr<RematchPeer>> peers; //!< peers[0] hosts.
			std::filesystem::path ticketDirectory;

			RematchPeer& Host() { return *peers.front(); }
			RematchPeer* Client(uint8_t lockstepId) {
				for (auto& peer: peers) {
					if (!peer->host && !peer->gone && peer->LockstepId() == lockstepId) return peer.get();
				}
				return nullptr;
			}
		};

		NetH4Identity RematchIdentity() {
			NetH4Identity identity;
			identity.controllerFrameVersion = ControllerFrame::c_Version;
			identity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
			identity.gameVersion = "7.0.0-test";
			identity.buildId = "rematch-roster-selftest";
			return identity;
		}

		uint64_t RematchUnixClock(void*) {
			return 1'700'000'000'000ULL;
		}

		// NetMatchService::QuerySeatState over the fixture host's plane.
		NetLockstepSeatState RematchSeatState(void* context, uint8_t lockstepPeerId, NetPeerId transportPeerId) {
			const auto* admission = static_cast<const NetReconnectHost*>(context);
			NetLockstepSeatState state;
			state.fencedTransport = transportPeerId != c_InvalidNetPeerId && admission->IsFenced(transportPeerId);
			state.heldForReclaim = admission->IsSeatHeldForReclaim(lockstepPeerId);
			return state;
		}

		// Admission attached as NetMatchService::AttachAdmissionPlane does, so each client holds a ticket.
		bool SetUpRematchFixture(RematchFixture& fixture, const std::string& label, uint16_t port, uint8_t peerCount, std::string* error) {
			std::error_code code;
			fixture.ticketDirectory = std::filesystem::current_path() / "Userdata" / "rematch-roster-selftest" / label;
			std::filesystem::remove_all(fixture.ticketDirectory, code);
			std::filesystem::create_directories(fixture.ticketDirectory, code);
			if (code) {
				*error = label + ": could not prepare the ticket directory: " + code.message();
				return false;
			}
			NetMatchConfig matchConfig = MakeConfig();
			matchConfig.peerCount = peerCount;
			matchConfig.players.clear();
			for (uint8_t peerId = 1; peerId <= peerCount; ++peerId) {
				matchConfig.players.push_back(NetMatchPlayerSlot{peerId, static_cast<uint8_t>(peerId - 1), false, peerId == 1 ? "Host" : "Client " + std::to_string(peerId)});
			}
			const NetH4Identity identity = RematchIdentity();
			for (uint8_t index = 0; index < peerCount; ++index) {
				auto peer = std::make_unique<RematchPeer>();
				peer->host = index == 0;
				peer->name = peer->host ? "host" : "client " + std::to_string(index);
				NetMatchRunnerConfig& config = peer->config;
				config.host = peer->host;
				config.joinAddress = peer->host ? "" : "loopback";
				config.matchConfig = matchConfig;
				config.useLobbyProtocol = true;
				config.sessionWaitMs = 8000;
				config.lobbyWaitMs = 4000;
				config.lockstepWaitMs = 4000;
				config.missingFrameGraceMs = 30000;
				config.postSessionSettleMs = config.postLobbySettleMs = 0;
				config.startFrame = 1;
				config.scenario = "rematch-roster-selftest";
				config.nowMs = [&fixture] { return fixture.clock.NowMs(); };
				NetSessionConfig& session = config.sessionConfig;
				session.port = port;
				session.maxPeers = static_cast<uint8_t>(peerCount - 1);
				session.timeoutMs = 30000;
				session.heartbeatIntervalMs = 25;
				session.sessionId = matchConfig.sessionId;
				session.localNonce += index;
				session.displayName = peer->host ? "Host" : "Client";
				NetIdentityManifest& manifest = session.localIdentity;
				manifest.gameVersion = identity.gameVersion;
				manifest.networkProtocolVersion = NetProtocol::c_Version;
				manifest.controllerFrameVersion = ControllerFrame::c_Version;
				manifest.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
				manifest.buildId = identity.buildId;
				manifest.platform = "test";
				if (peer->host) {
					if (!peer->registry.BeginHostedSession()) {
						*error = label + ": the seat-auth registry could not draw an epoch";
						return false;
					}
					peer->admission.Configure(&peer->registry, session.sessionId, identity);
					peer->admission.SetSeatTable(NetH4BuildSeatTable(matchConfig), matchConfig.mode);
					peer->admission.SetHostAddress("loopback");
					peer->admission.SetMatchConfigHash(NetMatchConfigUtil::HashConfig(matchConfig));
					peer->admission.SetLiveMatch(false);
					peer->session.SetReconnectHost(&peer->admission);
				} else {
					peer->store.SetPath((fixture.ticketDirectory / (std::to_string(index) + ".ticket")).string());
					peer->reconnect.Configure(&peer->store, identity, "Client " + std::to_string(index));
					peer->reconnect.SetUnixClock(&RematchUnixClock, nullptr);
					peer->reconnect.SetHostContext("loopback", NetHash32{});
					peer->session.SetReconnectClient(&peer->reconnect);
				}
				fixture.peers.push_back(std::move(peer));
			}
			return true;
		}

		// What NetMatchService::ConsumeReadyToLaunch wires into a round it launches.
		void AttachRematchRound(RematchPeer& peer) {
			NetLockstepCoordinator& round = *peer.round;
			round.DeferStopsToTickBoundary();
			if (peer.host) {
				round.SetSeatStateSource(&RematchSeatState, &peer.admission);
			}
			RematchPeer* owner = &peer;
			round.SetSessionEventSink([owner](const NetTransportEvent& event) { owner->handover.push_back(event); });
			peer.nextProduce = round.GetConfig().startFrame;
			peer.lastApplied = round.GetConfig().startFrame > 0 ? round.GetConfig().startFrame - 1 : 0;
		}

		// A worker per peer; a settled peer launches and keeps relaying starts, as the sim thread does.
		bool LaunchRematchRound(const std::vector<RematchPeer*>& peers, const std::function<bool(RematchPeer&, std::string*)>& setup, std::string* error) {
			for (RematchPeer* peer: peers) {
				peer->round = std::make_unique<NetLockstepCoordinator>();
				peer->handover.clear();
				peer->seats.reset();
				peer->trace.clear();
			}
			std::vector<std::string> errors(peers.size());
			std::vector<std::atomic<int>> outcome(peers.size()); // 0 setting up, 1 started, 2 failed
			std::vector<char> launched(peers.size(), 0);
			std::vector<std::thread> workers;
			for (size_t index = 0; index < peers.size(); ++index) {
				workers.emplace_back([&, index] { outcome[index].store(setup(*peers[index], &errors[index]) ? 1 : 2); });
				if (peers[index]->host) {
					for (int spin = 0; spin < 2000 && !peers[index]->transport.IsHosting(); ++spin) {
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
					}
				}
			}
			for (bool settingUp = true; settingUp;) {
				settingUp = false;
				for (size_t index = 0; index < peers.size(); ++index) {
					const int state = outcome[index].load();
					if (state == 0) {
						settingUp = true;
					} else if (state == 1) {
						if (!launched[index]) {
							AttachRematchRound(*peers[index]);
							launched[index] = 1;
						}
						peers[index]->round->Tick(NetLockstepNowMs());
					}
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			for (std::thread& worker: workers) {
				worker.join();
			}
			std::string failures;
			for (size_t index = 0; index < peers.size(); ++index) {
				if (outcome[index].load() != 1) {
					failures += (failures.empty() ? "" : "; ") + peers[index]->name + ": " + errors[index];
				}
			}
			if (!failures.empty()) {
				*error = failures;
				return false;
			}
			return true;
		}

		std::vector<RematchPeer*> LiveRematchPeers(RematchFixture& fixture) {
			std::vector<RematchPeer*> live;
			for (auto& peer: fixture.peers) {
				if (!peer->gone) live.push_back(peer.get());
			}
			return live;
		}

		// The host's roster publication, as NetMatchService::PublishModerationView builds it.
		void PublishRematchSeats(RematchPeer& host, uint64_t nowMs) {
			NetLockstepCoordinator& round = *host.round;
			if (!round.IsRunning()) return;
			const auto& leaves = round.GetPeerLeaveFrames();
			std::vector<NetSeatPresenceEntry> presence;
			for (const NetH4ModerationSeat& seat: host.admission.GetModerationView()) {
				if (seat.cpu || seat.lockstepPeerId == 0) continue;
				NetSeatPresenceEntry entry;
				entry.stableSeat = seat.stableSeat;
				entry.peerId = seat.lockstepPeerId;
				entry.holderGeneration = seat.holderGeneration;
				entry.seatGeneration = seat.seatGeneration;
				entry.incarnation = seat.incarnation;
				if (seat.closed) {
					entry.state = NetSeatPresenceState::Left;
				} else if (seat.dropped) {
					entry.state = seat.reclaiming ? NetSeatPresenceState::Reconnecting : NetSeatPresenceState::Disconnected;
					entry.holdActive = seat.heldForReclaim;
					entry.holdUntilMs = seat.holdUntilMs;
				} else if (!seat.substituteName.empty()) {
					entry.state = NetSeatPresenceState::Substituted;
				}
				const auto left = leaves.find(seat.lockstepPeerId);
				if (seat.dropped && left != leaves.end()) {
					entry.holdUntilFrame = left->second + NetLockstepCoordinator::c_ReclaimHoldFrames;
				}
				presence.push_back(std::move(entry));
			}
			std::sort(presence.begin(), presence.end(), [](const auto& a, const auto& b) { return a.stableSeat < b.stableSeat; });
			(void)round.PublishSeatSnapshot(std::move(presence), nowMs);
		}

		ControllerFrame RematchFrame(uint8_t peerId, uint64_t frame) {
			ControllerFrame controller;
			controller.actorUniqueID = 1000 + peerId;
			controller.stateMask = frame;
			return controller;
		}

		// One committed tick as every peer must see it: each sender's frames under its id.
		std::string DescribeRematchFrame(const NetLockstepReadyFrame& ready, uint8_t localPeerId) {
			std::map<uint8_t, std::vector<const ControllerFrame*>> bySender;
			for (const ControllerFrame& frame: ready.localFrames) {
				bySender[localPeerId].push_back(&frame);
			}
			size_t offset = 0;
			for (const auto& [peerId, count]: ready.remoteFrameCounts) {
				for (size_t index = 0; index < count && offset < ready.remoteFrames.size(); ++index) {
					bySender[peerId].push_back(&ready.remoteFrames[offset++]);
				}
			}
			std::ostringstream text;
			text << ready.frame;
			for (const auto& [peerId, frames]: bySender) {
				text << ' ' << static_cast<int>(peerId) << ':';
				for (const ControllerFrame* frame: frames) {
					text << frame->actorUniqueID << '/' << frame->stateMask << ',';
				}
			}
			text << " commands=" << ready.localCommands.size() + ready.remoteCommands.size();
			return text.str();
		}

		// NetMatchService::PumpSessionEvents, then one pass of the sim thread's lockstep loop.
		void PumpRematchPeer(RematchFixture& fixture, RematchPeer& peer) {
			if (peer.gone || peer.leaving || !peer.round) return;
			NetLockstepCoordinator& round = *peer.round;
			const uint64_t nowMs = fixture.clock.NowMs();
			if (peer.host) {
				peer.admission.SetLiveMatch(true);
				peer.session.SetLockstepFrame(round.GetStats().nextFrame);
				peer.session.TickAdmissionPlane(nowMs);
			}
			std::vector<NetTransportEvent> events;
			events.swap(peer.handover);
			for (const NetTransportEvent& event: events) {
				peer.session.InjectEvent(event, nowMs);
			}
			if (peer.host) {
				(void)peer.admission.TakePendingReseats();
				for (const NetHoldResolutionNotice& notice: peer.admission.TakePendingHoldResolutions()) {
					NetLockstepHoldResolution resolution = NetLockstepHoldResolution::None;
					switch (notice.resolution) {
						case NetHoldResolution::Expired: resolution = NetLockstepHoldResolution::Expired; break;
						case NetHoldResolution::Reclaimed: resolution = NetLockstepHoldResolution::Reclaimed; break;
						case NetHoldResolution::Substituted: resolution = NetLockstepHoldResolution::Substituted; break;
					}
					round.ResolveHeldSeat(notice.lockstepPeerId, resolution, nowMs);
				}
				PublishRematchSeats(peer, nowMs);
			} else if (std::optional<NetLockstepSeatSnapshot> snapshot = round.TakeSeatSnapshot()) {
				peer.seats = std::move(snapshot);
			}
			std::string ignored;
			if (round.IsRunning() && round.NeedsResyncPriming()) {
				(void)round.PrimeResyncInputs({}, &ignored);
			}
			while (round.IsRunning() && !round.NeedsResyncPriming() && peer.nextProduce <= peer.lastApplied + 2 &&
			       round.QueueLocalInput(peer.nextProduce, {RematchFrame(round.GetConfig().localPeerId, peer.nextProduce)}, {}, &ignored)) {
				++peer.nextProduce;
			}
			round.Tick(NetLockstepNowMs());
			NetLockstepReadyFrame ready;
			while (round.PopReadyFrame(ready)) {
				peer.trace[ready.frame] = DescribeRematchFrame(ready, round.GetConfig().localPeerId);
				peer.lastApplied = ready.frame;
				(void)round.FinishSimulationTick(ready.frame);
			}
			if (round.IsRunning()) {
				(void)round.WaivePendingPeersWhileWaiting(peer.lastApplied + 1);
			}
		}

		bool PumpRematchUntil(RematchFixture& fixture, uint32_t budgetMs, const std::function<bool()>& done) {
			const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
			while (true) {
				for (auto& peer: fixture.peers) {
					PumpRematchPeer(fixture, *peer);
				}
				if (done()) return true;
				if (std::chrono::steady_clock::now() >= until) return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		// Plays until every live peer has applied this many more ticks of its round.
		bool PlayRematchTicks(RematchFixture& fixture, uint64_t ticks) {
			std::map<RematchPeer*, uint64_t> target;
			for (RematchPeer* peer: LiveRematchPeers(fixture)) {
				target[peer] = peer->lastApplied + ticks;
			}
			return PumpRematchUntil(fixture, 4000, [&] {
				return std::all_of(target.begin(), target.end(), [](const auto& entry) { return entry.first->lastApplied >= entry.second; });
			});
		}

		// LeaveMatch: the §7 exchange while the link is up, then the round is told, then the process goes.
		bool LeaveRematchRound(RematchFixture& fixture, RematchPeer& leaver, std::string* error) {
			const uint8_t leaverId = leaver.LockstepId();
			std::string leaveError;
			leaver.leaving = true;
			if (!leaver.reconnect.BeginLeave(fixture.clock.NowMs(), &leaveError)) {
				*error = leaver.name + " could not start its leave: " + leaveError;
				return false;
			}
			(void)PumpRematchUntil(fixture, 3000, [&] {
				leaver.session.Tick(fixture.clock.NowMs());
				leaver.reconnect.Tick(fixture.clock.NowMs());
				return leaver.reconnect.GetState() != NetH4ClientState::Leaving || leaver.reconnect.WantsLinkClosed();
			});
			if (leaver.reconnect.GetState() != NetH4ClientState::Left) {
				*error = leaver.name + "'s leave was not acknowledged: " + NetReconnectClientStateName(leaver.reconnect.GetState());
				return false;
			}
			leaver.round->Leave("player left");
			const bool recorded = PumpRematchUntil(fixture, 3000, [&] {
				for (RematchPeer* peer: LiveRematchPeers(fixture)) {
					if (peer != &leaver && !peer->round->GetPeerLeaveFrames().contains(leaverId)) return false;
				}
				return true;
			});
			if (!recorded) {
				*error = "the round never recorded " + leaver.name + "'s leave";
				return false;
			}
			leaver.gone = true;
			leaver.transport.Stop();
			NetSession& hostSession = fixture.Host().session;
			const bool closed = PumpRematchUntil(fixture, 3000, [&] {
				return hostSession.GetReadyPeerCount() == LiveRematchPeers(fixture).size() - 1;
			});
			if (!closed) {
				*error = "the host session still holds " + leaver.name;
				return false;
			}
			return true;
		}

		// FinishMatch on the host; ReturnToLobby then hands each session what its stopped round held.
		bool FinishRematchRound(RematchFixture& fixture, std::string* error) {
			fixture.Host().round->Complete("round over");
			const bool stopped = PumpRematchUntil(fixture, 4000, [&] {
				const auto live = LiveRematchPeers(fixture);
				return std::none_of(live.begin(), live.end(), [](RematchPeer* peer) { return peer->round->IsRunning(); });
			});
			if (!stopped) {
				*error = "a round was still running after the host completed it";
				return false;
			}
			for (RematchPeer* peer: LiveRematchPeers(fixture)) {
				std::vector<NetTransportEvent> events;
				events.swap(peer->handover);
				for (const NetTransportEvent& event: events) {
					peer->session.InjectEvent(event, fixture.clock.NowMs());
				}
			}
			return true;
		}

		// What a client's ReturnToLobby hands its runner: the played roster minus its round's leaves.
		std::vector<uint8_t> ClientRematchSurvivors(const RematchPeer& client) {
			const NetMatchConfig& played = client.runner.GetMatchConfig();
			std::vector<uint8_t> survivors{played.hostPeerId};
			for (const NetMatchPlayerSlot& slot: played.players) {
				if (!slot.cpu && slot.peerId != 0 && slot.peerId != played.hostPeerId && !client.round->GetPeerLeaveFrames().contains(slot.peerId)) {
					survivors.push_back(slot.peerId);
				}
			}
			return survivors;
		}

		// ReturnToLobby then StartNextMatch on every live peer.
		bool RematchFixtureRound(RematchFixture& fixture, std::string* error) {
			const std::vector<RematchPeer*> live = LiveRematchPeers(fixture);
			for (RematchPeer* peer: live) {
				if (peer->host) {
					peer->runner.SetStartFrame(1);
				} else {
					peer->runner.SetRematchRoster(ClientRematchSurvivors(*peer));
				}
			}
			return LaunchRematchRound(live, [](RematchPeer& peer, std::string* setupError) {
				return peer.runner.StartNextMatch(peer.transport, peer.session, *peer.round, setupError);
			}, error);
		}

		void StopRematchFixture(RematchFixture& fixture) {
			for (RematchPeer* peer: LiveRematchPeers(fixture)) {
				if (peer->round) peer->round->Complete("fixture over");
				peer->transport.Stop();
			}
		}

		// Each client's stable seat, on the host's plane and in its seat view, is its round-1 ticket's.
		bool CheckRematchStableSeats(RematchFixture& fixture, const std::map<RematchPeer*, uint16_t>& roundOneSeats, std::string& details, std::string* error) {
			RematchPeer& host = fixture.Host();
			const std::vector<NetH4SeatStatus> statuses = host.admission.GetSeatStatuses();
			const auto statusOf = [&statuses](uint8_t lockstepId) {
				return std::find_if(statuses.begin(), statuses.end(), [lockstepId](const NetH4SeatStatus& status) { return status.lockstepPeerId == lockstepId; });
			};
			const auto hostSeat = statusOf(host.LockstepId());
			if (hostSeat == statuses.end() || hostSeat->stableSeat != 0) {
				*error = "the host's own seat left stable seat 0";
				return false;
			}
			const bool shown = PumpRematchUntil(fixture, 3000, [&] {
				for (RematchPeer* peer: LiveRematchPeers(fixture)) {
					if (!peer->host && (!peer->seats || peer->seats->roundId != peer->round->GetRoundId())) return false;
				}
				return true;
			});
			if (!shown) {
				*error = "a client was never shown this round's seat view";
				return false;
			}
			for (RematchPeer* peer: LiveRematchPeers(fixture)) {
				if (peer->host) continue;
				const uint16_t expected = roundOneSeats.at(peer);
				const uint8_t lockstepId = peer->LockstepId();
				const auto status = statusOf(lockstepId);
				if (status == statuses.end() || status->stableSeat != expected || !status->committed || status->closed || status->dropped) {
					*error = "the host's plane seats lockstep peer " + std::to_string(lockstepId) + " on stable seat " +
					         (status == statuses.end() ? std::string("none") : std::to_string(status->stableSeat) + (status->closed ? " (closed)" : "") + (!status->committed ? " (uncommitted)" : "")) +
					         ", its round-1 ticket names stable seat " + std::to_string(expected);
					return false;
				}
				NetPeerId connection = c_InvalidNetPeerId;
				uint32_t generation = 0;
				uint32_t incarnation = 0;
				NetPeerId expectedConnection = c_InvalidNetPeerId;
				for (const NetSessionPeerInfo& ready: host.session.GetReadyPeers()) {
					if (ready.assignedPeerId == peer->session.GetLocalPeerId()) expectedConnection = ready.transportPeerId;
				}
				if (!host.admission.GetSeatHolder(expected, connection, generation, incarnation) || connection != expectedConnection ||
				    generation != peer->reconnect.GetRecord().holderGeneration) {
					*error = "stable seat " + std::to_string(expected) + " does not hold lockstep peer " + std::to_string(lockstepId) + "'s own holder state";
					return false;
				}
				const auto shownSeat = std::find_if(peer->seats->seats.begin(), peer->seats->seats.end(), [lockstepId](const NetSeatPresenceEntry& entry) { return entry.peerId == lockstepId; });
				if (shownSeat == peer->seats->seats.end() || shownSeat->stableSeat != expected || shownSeat->state != NetSeatPresenceState::Present) {
					*error = "lockstep peer " + std::to_string(lockstepId) + " was shown stable seat " +
					         (shownSeat == peer->seats->seats.end() ? std::string("none") : std::to_string(shownSeat->stableSeat) + " " + NetSeatPresence::StateName(shownSeat->state)) +
					         ", its round-1 ticket names stable seat " + std::to_string(expected);
					return false;
				}
				details += " lockstep" + std::to_string(lockstepId) + "=seat" + std::to_string(expected) + "/gen" + std::to_string(generation);
			}
			return true;
		}

		// Two clean leaves, a rematch after each: the survivor that moves up twice must keep its seat.
		bool TestRematchKeepsStableSeatsAcrossTwoShrinks(std::string* error) {
			RematchFixture fixture;
			if (!SetUpRematchFixture(fixture, "two-shrinks", 43150, 4, error)) return false;
			std::string step;
			auto fail = [&](const std::string& what) {
				*error = "rematch stable seats across two shrinks: " + what + (step.empty() ? "" : ": " + step);
				StopRematchFixture(fixture);
				return false;
			};
			if (!LaunchRematchRound(LiveRematchPeers(fixture), [](RematchPeer& peer, std::string* setupError) {
				    return peer.runner.Start(peer.transport, peer.session, *peer.round, peer.config, setupError);
			    }, &step)) {
				return fail("round 1 setup failed");
			}
			if (!PlayRematchTicks(fixture, 8)) return fail("round 1 never committed");
			std::map<RematchPeer*, uint16_t> roundOneSeats;
			for (RematchPeer* peer: LiveRematchPeers(fixture)) {
				if (peer->host) continue;
				const std::vector<NetH4SeatStatus> statuses = fixture.Host().admission.GetSeatStatuses();
				const auto status = std::find_if(statuses.begin(), statuses.end(), [peer](const NetH4SeatStatus& entry) { return entry.lockstepPeerId == peer->LockstepId(); });
				if (status == statuses.end() || !peer->reconnect.HasRecord() || status->stableSeat != peer->reconnect.GetRecord().stableSeat) {
					return fail("round 1 did not seat lockstep peer " + std::to_string(peer->LockstepId()) + " on the stable seat its ticket names");
				}
				roundOneSeats[peer] = status->stableSeat;
			}
			RematchPeer* survivor = fixture.Client(4);
			RematchPeer* firstLeaver = fixture.Client(3);
			if (!survivor || !firstLeaver) return fail("round 1 did not seat lockstep peers 3 and 4");
			if (!LeaveRematchRound(fixture, *firstLeaver, &step) || !PlayRematchTicks(fixture, 4) || !FinishRematchRound(fixture, &step)) {
				return fail("the first clean leave did not settle");
			}
			if (!RematchFixtureRound(fixture, &step)) return fail("the first rematch did not relaunch");
			if (fixture.Host().runner.GetMatchConfig().peerCount != 3 || survivor->LockstepId() != 3) {
				return fail("the first rematch is not the three survivors with the last one on lockstep peer 3");
			}
			std::string details;
			if (!PlayRematchTicks(fixture, 8) || !CheckRematchStableSeats(fixture, roundOneSeats, details, &step)) {
				return fail("after the first rematch");
			}
			RematchPeer* secondLeaver = fixture.Client(2);
			if (!secondLeaver) return fail("the first rematch has no lockstep peer 2");
			if (!LeaveRematchRound(fixture, *secondLeaver, &step) || !PlayRematchTicks(fixture, 4) || !FinishRematchRound(fixture, &step)) {
				return fail("the second clean leave did not settle");
			}
			if (!RematchFixtureRound(fixture, &step)) return fail("the second rematch did not relaunch");
			if (fixture.Host().runner.GetMatchConfig().peerCount != 2 || survivor->LockstepId() != 2) {
				return fail("the second rematch is not the two survivors with the last one on lockstep peer 2");
			}
			details.clear();
			if (!PlayRematchTicks(fixture, 8) || !CheckRematchStableSeats(fixture, roundOneSeats, details, &step)) {
				return fail("after the second rematch");
			}
			std::cout << "PASS rematch_stable_seats_two_shrinks peer_count=2 host=seat0" << details << " (round-1 lockstep 4)" << std::endl;
			StopRematchFixture(fixture);
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

		bool TestServiceDedicatedRequest(std::string* error) {
			for (const uint16_t port : {uint16_t(0), uint16_t(41010)}) {
				NetMatchService service;
				NetMatchServiceRequest request;
				request.host = false;
				request.dedicated = true;
				request.port = port;
				std::string startError;
				if (service.Start(request, &startError) || startError != "dedicated service requires the host role") {
					*error = "a dedicated join request was not refused: " + startError;
					return false;
				}
			}
			NetMatchService service;
			const std::string report = service.BuildReportJson();
			if (report.find("\"dedicated\":false") == std::string::npos || report.find("\"human_seats\"") == std::string::npos) {
				*error = "service report is missing the dedicated/human_seats fields";
				return false;
			}
			return true;
		}

		bool TestLobbyThreePeerDedicated(std::string* error) {
			const uint16_t port = 43009;
			LoopbackTransport hostT, clientAT, clientBT;
			if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) || !clientBT.Connect("loopback", port, error)) {
				return false;
			}
			// The dedicated host is lockstep peer 1 with no roster slot; peers 2 and 3 are the humans.
			NetMatchConfig matchConfig = MakeConfig();
			matchConfig.peerCount = 3;
			matchConfig.dedicated = true;
			matchConfig.players = {
			    NetMatchPlayerSlot{2, 0, false, "Client A"},
			    NetMatchPlayerSlot{3, 1, false, "Client B"},
			};
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
					*error = "dedicated three-peer lobby failed; host=" + std::string(NetLobbySession::StateName(host.GetState())) +
					         " a=" + NetLobbySession::StateName(clientA.GetState()) + " b=" + NetLobbySession::StateName(clientB.GetState());
					return false;
				}
				hostT.AdvanceTimeMs(10);
				clientAT.AdvanceTimeMs(10);
				clientBT.AdvanceTimeMs(10);
			}
			if (!host.IsStarted() || !clientA.IsStarted() || !clientB.IsStarted()) {
				*error = "dedicated lobby did not reach Started on every peer";
				return false;
			}
			if (host.GetMatchConfigHash() != clientA.GetMatchConfigHash() || host.GetMatchConfigHash() != clientB.GetMatchConfigHash() ||
			    !clientA.GetMatchConfig().dedicated || !clientB.GetMatchConfig().dedicated) {
				*error = "dedicated lobby did not converge on the dedicated config";
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

	// A seat with no roster name is "Player N"; one with a name is the name. Never both.
	bool TestRosterBannerNamesThePlayerOnce(std::string* error) {
		NetMatchService service;
		service.m_State = NetMatchServiceState::Running;
		NetLobbyMember host;
		host.peerId = 1;
		host.displayName = "Host";
		NetLobbyMember nameless;
		nameless.peerId = 3;
		service.m_LobbySnapshot.members = {host, nameless};
		NetLockstepSeatSnapshot snapshot;
		snapshot.senderPeerId = 1;
		snapshot.sessionId = 12;
		snapshot.roundId = 1;
		snapshot.revision = 1;
		snapshot.observedAtMs = 1000;
		NetSeatPresenceEntry seat;
		seat.stableSeat = 2;
		seat.peerId = 3;
		seat.state = NetSeatPresenceState::Present;
		snapshot.seats = {seat};
		if (!service.m_SeatPresence.ApplySnapshot(snapshot, 1000)) {
			*error = "the present snapshot was not applied";
			return false;
		}
		service.RecordRosterTransitions(snapshot.observedAtMs);
		snapshot.revision = 2;
		snapshot.observedAtMs = 2000;
		snapshot.seats[0].state = NetSeatPresenceState::Left;
		if (!service.m_SeatPresence.ApplySnapshot(snapshot, 2000)) {
			*error = "the leave snapshot was not applied";
			return false;
		}
		const size_t before = ScenarioRunner::GetNetUiToastLog().size();
		service.RecordRosterTransitions(snapshot.observedAtMs);
		const std::vector<ScenarioRunner::NetUiToastRecord>& toasts = ScenarioRunner::GetNetUiToastLog();
		if (toasts.size() != before + 1 || toasts.back().kind != "player_left") {
			*error = "the leave banner was not recorded";
			return false;
		}
		if (toasts.back().text != "Player 3 left") {
			*error = "the leave banner named the player as \"" + toasts.back().text + "\"";
			return false;
		}
		return true;
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
		const size_t toastsBefore = ScenarioRunner::GetNetUiToastLog().size();
		service.RecordRosterTransitions(snapshot.observedAtMs);
		// The banner is pushed from here with no managers built, so it has no sim clock to read.
		const std::vector<ScenarioRunner::NetUiToastRecord>& toasts = ScenarioRunner::GetNetUiToastLog();
		if (toasts.size() != toastsBefore + 1 || toasts.back().kind != "player_rejoined" || toasts.back().tick != 0) {
			*error = "the rejoin banner was not recorded for the returning seat";
			return false;
		}
		// The roster's own name, once - never "Player Leaver".
		if (toasts.back().text != "Leaver rejoined") {
			*error = "the rejoin banner named the player as \"" + toasts.back().text + "\"";
			return false;
		}
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

	namespace {
		NetLockstepSeatState FencedSeatState(void*, uint8_t, NetPeerId) {
			NetLockstepSeatState state;
			state.fencedTransport = true;
			return state;
		}
	}

	bool TestPendingSessionEventSurvivesTeardown(std::string* error) {
		const uint16_t port = 43219;
		LoopbackTransport hostTransport, clientTransport;
		NetMatchService service;
		service.m_IsHost = true;
		service.m_State = NetMatchServiceState::Running;
		service.m_Runner = std::make_unique<NetMatchRunner>();
		service.m_Session = std::make_unique<NetSession>();
		NetSession client;
		NetSessionConfig hostConfig;
		hostConfig.port = port;
		hostConfig.displayName = "Host";
		hostConfig.maxPeers = 1;
		hostConfig.heartbeatIntervalMs = 25;
		NetIdentityManifest& identity = hostConfig.localIdentity;
		identity.gameVersion = "7.0.0-test";
		identity.networkProtocolVersion = NetProtocol::c_Version;
		identity.controllerFrameVersion = ControllerFrame::c_Version;
		identity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
		identity.buildId = "pending-session-event-selftest";
		identity.platform = "test";
		NetSessionConfig clientConfig = hostConfig;
		clientConfig.displayName = "Client";
		++clientConfig.localNonce;
		if (!service.m_Session->StartHost(hostTransport, hostConfig, error) ||
		    !client.StartClient(clientTransport, "loopback", clientConfig, error)) {
			return false;
		}
		for (uint64_t now = 0; now <= 2000 && service.m_Session->GetReadyPeerCount() != 1; now += 10) {
			service.m_Session->Tick(now);
			client.Tick(now);
			hostTransport.AdvanceTimeMs(10);
			clientTransport.AdvanceTimeMs(10);
		}
		if (service.m_Session->GetReadyPeerCount() != 1) {
			*error = "the teardown fixture never seated the client on the host session";
			return false;
		}
		const NetPeerId fencedTransportPeer = service.m_Session->GetReadyPeers().front().transportPeerId;

		// A relay host mid-round: the coordinator owns the transport queue and hands session traffic over.
		service.m_Coordinator = std::make_unique<NetLockstepCoordinator>();
		NetLockstepCoordinator& coordinator = *service.m_Coordinator;
		coordinator.m_RelayHost = true;
		coordinator.m_State = NetLockstepState::Running;
		coordinator.m_RemotePeerIds = {2};
		coordinator.m_RemoteTransports[2] = fencedTransportPeer;
		coordinator.m_Stats.peerFramesWaived = 1;
		coordinator.SetSeatStateSource(&FencedSeatState, nullptr);
		service.AttachCoordinatorSessionSink();
		const NetTransportEvent closed{NetTransportEventType::PeerDisconnected, fencedTransportPeer,
		                               NetTransportLane::ControlReliable, {}, "seat reclaimed by a newer connection"};
		coordinator.HandleEvent(closed, 0);
		if (service.m_PendingSessionEvents.size() != 1) {
			*error = "the coordinator did not hand the fenced disconnect to the service queue";
			return false;
		}

		// The teardown the resync runs: the coordinator dies and takes the queue's only reader with it.
		service.ReportRuntimeError("teardown with a session event pending");
		nlohmann::json report;
		try {
			report = nlohmann::json::parse(service.BuildReportJson());
		} catch (const nlohmann::json::exception& parseError) {
			*error = std::string("teardown report was not JSON: ") + parseError.what();
			return false;
		}
		const nlohmann::json peers = report.value("runner", nlohmann::json::object())
		                                 .value("session", nlohmann::json::object())
		                                 .value("peers", nlohmann::json::array());
		std::string peerState = "absent";
		for (const nlohmann::json& peer: peers) {
			if (peer.value("transport_peer_id", 0ULL) == static_cast<uint64_t>(fencedTransportPeer)) {
				peerState = peer.value("state", "");
			}
		}
		if (peerState != "Closed") {
			*error = "event lost: the session never saw the fenced disconnect the coordinator took off the "
			         "transport; peer " + std::to_string(fencedTransportPeer) + " is " + peerState;
			return false;
		}
		const nlohmann::json events = report.value("session_events", nlohmann::json::object());
		if (events.value("drained_at_teardown", 0) != 1 || events.value("discarded", 0) != 0) {
			*error = "event lost: teardown drain counters read " + events.dump();
			return false;
		}
		const nlohmann::json totals = report.value("lockstep_totals", nlohmann::json::object());
		if (totals.value("peer_frames_waived", 0) != 1) {
			*error = "the waiver count died with the coordinator; lockstep_totals=" + totals.dump();
			return false;
		}
		if (!service.m_PendingSessionEvents.empty()) {
			*error = "the teardown left the handover queue filled";
			return false;
		}
		std::cout << "PASS pending_session_event_survives_teardown peer_state=Closed drained=1 discarded=0 peer_frames_waived=1" << std::endl;
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
		if (!TestMatchConfigDedicated(&error)) return fail(error);
		if (!TestReplayCommandSenders(&error)) return fail(error);
		if (!TestOwnershipPolicies(&error)) return fail(error);
		if (!TestLockstepCoordinatorUsesMatchOwnership(&error)) return fail(error);
		if (!TestLobbyCodecRoundTrips(&error)) return fail(error);
		if (!TestMalformedLobbyPayloads(&error)) return fail(error);
		if (!TestLobbyCodecDedicatedFlag(&error)) return fail(error);
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
		if (!TestRematchRosterDerivation(&error)) return fail(error);
		if (!TestRematchRebuildsTheSurvivingRoster(&error)) return fail(error);
		// Each rematch-roster case reports its own verdict, so one red case cannot hide another.
		std::string twoShrinksError;
		if (!TestRematchKeepsStableSeatsAcrossTwoShrinks(&twoShrinksError)) {
			std::cerr << "[net-match-selftest] FAIL: " << twoShrinksError << std::endl;
		}
		if (!twoShrinksError.empty()) return fail(twoShrinksError);
		if (!TestLobbyThreePeer(&error)) return fail(error);
		if (!TestServiceDedicatedRequest(&error)) return fail(error);
		if (!TestLobbyThreePeerDedicated(&error)) return fail(error);
		if (!TestLobbyLateJoinerRosterRace(&error)) return fail(error);
		if (!TestServiceRuntimeErrorSurface(&error)) return fail(error);
		if (!TestJoinWaitTrigger(&error)) return fail(error);
		if (!TestSaveCompressionChoice(&error)) return fail(error);
		if (!TestResyncReportAbsentWhenIdle(&error)) return fail(error);
		std::string failedReportError;
		std::string rejoinOverError;
		std::string rejoinWaitError;
		std::string tickClockError;
		std::string capTickError;
		std::string executedTickError;
		std::string earlyOverTickError;
		std::string healedEndError;
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
		if (!TestEarlyOverUsesMatchTick(&earlyOverTickError)) {
			std::cerr << "[net-match-selftest] FAIL: " << earlyOverTickError << std::endl;
		}
		if (!TestHealedRoundPlannedEndIsNotAFailure(&healedEndError)) {
			std::cerr << "[net-match-selftest] FAIL: " << healedEndError << std::endl;
		}
		if (!failedReportError.empty()) return fail(failedReportError);
		if (!rejoinOverError.empty()) return fail(rejoinOverError);
		if (!rejoinWaitError.empty()) return fail(rejoinWaitError);
		if (!tickClockError.empty()) return fail(tickClockError);
		if (!capTickError.empty()) return fail(capTickError);
		if (!executedTickError.empty()) return fail(executedTickError);
		if (!earlyOverTickError.empty()) return fail(earlyOverTickError);
		if (!healedEndError.empty()) return fail(healedEndError);
		if (!TestHoldResolutionPumpDoesNotRelock(&error)) return fail(error);
		if (!TestRosterTransitionsRecordHoldThenPresent(&error)) return fail(error);
		if (!TestRosterBannerNamesThePlayerOnce(&error)) return fail(error);
		if (!TestPendingSessionEventSurvivesTeardown(&error)) return fail(error);

		std::cout << "[net-match-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
