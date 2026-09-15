#include "NetMatchSelfTest.h"

#include "NetActorOwnership.h"
#include "GnsTransport.h"
#include "NetMuxTransport.h"
#include "SettingsMan.h"
#ifdef CCCP_WITH_GNS
#include "GnsSignaling.h"
#endif
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
#include <deque>
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
			NetMatchConfig historical = MakeConfig();
			historical.version = 2;
			std::vector<uint8_t> historicalWire;
			if (!NetLobbyProtocol::Encode({NetLobbyMatchConfig{historical}}, historicalWire)) return false;
			historicalWire[4] = 3;
			const auto networkDecode = NetLobbyProtocol::Decode(historicalWire);
			if (networkDecode.ok || networkDecode.error.code != NetLobbyErrorCode::UnsupportedVersion) {
				*error = "network decode accepted a recorded v3 config envelope";
				return false;
			}
			std::vector<uint8_t> historicalReplay(original.begin(), original.begin() + 12);
			write32(historicalReplay, 8, static_cast<uint32_t>(historicalWire.size()));
			historicalReplay.insert(historicalReplay.end(), historicalWire.begin(), historicalWire.end());
			historicalReplay.insert(historicalReplay.end(), original.begin() + recordOffset, original.end());
			if (!writeFile(historicalReplay) || !reader.Open(path.string(), error) || reader.GetConfig() != historical ||
			    !reader.ReadFrame(decoded, eof, error) || decoded != first) {
				*error = "recorded v3 lobby/v2 config did not retain its defaults and commands";
				return false;
			}
			reader.Close();
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
			if (!NetMatchConfigUtil::ValidateLocalAlpha(noClients, &validationError)) {
				*error = "dedicated CPU roster was refused: " + validationError;
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

		bool TestCPURosterRequests(std::string* error) {
			for (bool host : {false, true}) {
				for (bool dedicated : {false, true}) {
					for (NetMatchMode mode : {NetMatchMode::PvPSkirmish, NetMatchMode::CoopPvE, NetMatchMode::PvPvE}) {
						NetMatchServiceRequest request;
						request.host = host;
						request.dedicated = dedicated;
						request.peerCount = dedicated ? 3 : 2;
						request.humans = 2;
						request.cpuSlots = 2;
						request.mode = mode;
						NetMatchConfig config;
						if (!NetMatchService::BuildMatchConfig(request, 123, config, error)) return false;
						const uint8_t firstCPU = mode == NetMatchMode::CoopPvE ? 1 : 2;
						if (config.players.size() != 4 || config.mode != mode ||
						    config.players[0].peerId != (dedicated ? 2 : 1) || config.players[0].cpu ||
						    config.players[1].team != (mode == NetMatchMode::CoopPvE ? 0 : 1) ||
						    config.players[2] != NetMatchPlayerSlot{0, firstCPU, true, "CPU 1"} ||
						    config.players[3] != NetMatchPlayerSlot{0, static_cast<uint8_t>(firstCPU + 1), true, "CPU 2"}) {
							*error = "authored CPU roster differs: " + NetMatchConfigUtil::BuildReportJson(config);
							return false;
						}
						if (!RoundTrip(NetLobbyMatchConfig{config}, error)) return false;
						request.humans = request.peerCount + 1;
						std::string reason;
						if (NetMatchService::BuildMatchConfig(request, 123, config, &reason) || reason != "human seats exceed peer capacity") {
							*error = "human capacity refusal differs: " + reason;
							return false;
						}
					}
				}
				NetMatchServiceRequest request;
				request.host = host;
				request.peerCount = 4;
				request.mode = NetMatchMode::PvPvE;
				NetMatchConfig config;
				std::string reason;
				if (NetMatchService::BuildMatchConfig(request, 123, config, &reason) || reason != "four-human PvPvE exceeds team capacity") {
					*error = "four-human PvPvE refusal differs: " + reason;
					return false;
				}
				request.humans = 0;
				request.cpuSlots = 2;
				if (NetMatchService::BuildMatchConfig(request, 123, config, &reason) || reason != "zero human seats require a dedicated host") {
					*error = "zero-human refusal differs: " + reason;
					return false;
				}
				request.dedicated = true;
				if (!NetMatchService::BuildMatchConfig(request, 123, config, error) || config.peerCount != 1 || config.players.size() != 2 ||
				    !RoundTrip(NetLobbyMatchConfig{config}, error)) return false;
				// The widest roster the wire carries: four co-op humans beside the three CPU teams left.
				NetMatchServiceRequest crowded;
				crowded.host = host;
				crowded.peerCount = 4;
				crowded.humans = 4;
				crowded.cpuSlots = 3;
				crowded.mode = NetMatchMode::CoopPvE;
				if (!NetMatchService::BuildMatchConfig(crowded, 123, config, error)) return false;
				if (config.players.size() != NetMatchConfigUtil::c_MaxPlayers) {
					*error = "the full co-op roster differs: " + NetMatchConfigUtil::BuildReportJson(config);
					return false;
				}
				if (!RoundTrip(NetLobbyMatchConfig{config}, error)) return false;
				NetMatchConfig oversize = config;
				oversize.players.push_back({0, 3, true, "CPU 4"});
				if (NetMatchConfigUtil::ValidateLocalAlpha(oversize, &reason) || reason != "player slot count is out of range") {
					*error = "player slot capacity refusal differs: " + reason;
					return false;
				}
				std::vector<uint8_t> oversizeBytes;
				NetLobbyError oversizeError;
				// The encoder validates first, so an oversize roster is refused there, by the validator's reason.
				if (NetLobbyProtocol::Encode({NetLobbyMatchConfig{oversize}}, oversizeBytes, &oversizeError) ||
				    oversizeError.code != NetLobbyErrorCode::InvalidValue || oversizeError.message != "player slot count is out of range") {
					*error = "the lobby codec accepted a roster past the slot capacity: " + oversizeError.message;
					return false;
				}
				// The AI-only dedicated match the battery launches: no human seat, one lockstep peer.
				NetMatchServiceRequest aiOnly;
				aiOnly.host = host;
				aiOnly.dedicated = true;
				aiOnly.peerCount = 2;
				aiOnly.humans = 0;
				aiOnly.cpuSlots = 2;
				aiOnly.mode = NetMatchMode::CoopPvE;
				if (!NetMatchService::BuildMatchConfig(aiOnly, 123, config, error)) return false;
				if (config.peerCount != 1 || config.players.size() != 2 ||
				    config.players[0] != NetMatchPlayerSlot{0, 1, true, "CPU 1"} ||
				    config.players[1] != NetMatchPlayerSlot{0, 2, true, "CPU 2"}) {
					*error = "authored AI-only roster differs: " + NetMatchConfigUtil::BuildReportJson(config);
					return false;
				}
				if (!RoundTrip(NetLobbyMatchConfig{config}, error)) return false;
			}
			std::cout << "[net-match-selftest] PASS cpu_roster_requests" << std::endl;
			return true;
		}

		bool TestCPURosterValidation(std::string* error) {
			NetMatchConfig config = MakeConfig();
			config.players.push_back({0, 2, true, "CPU 1"});
			config.players.push_back({0, 3, true, "CPU 2"});
			if (!RoundTrip(NetLobbyMatchConfig{config}, error)) return false;
			for (const auto& [team, expected] : std::vector<std::pair<uint8_t, std::string>>{
			         {0, "cpu slot shares a human team"}, {2, "duplicate cpu team"}, {4, "player team is out of range"}}) {
				NetMatchConfig invalid = config;
				invalid.players.back().team = team;
				for (bool host : {true, false}) {
					std::string reason;
					NetLobbySession lobby;
					LoopbackTransport transport;
					NetLobbySessionConfig setup;
					setup.host = host;
					setup.localPeerId = host ? 1 : 2;
					setup.remotePeerId = host ? 2 : 1;
					setup.remoteTransportPeerId = 1;
					setup.matchConfig = invalid;
					if (lobby.Start(transport, setup, &reason) || reason != expected) {
						*error = "CPU roster refusal differs on " + std::string(host ? "host: " : "client: ") + reason;
						return false;
					}
				}
			}
			const auto report = nlohmann::json::parse(NetMatchConfigUtil::BuildReportJson(config));
			if (std::count_if(report["players"].begin(), report["players"].end(), [](const auto& slot) { return slot["cpu"] == true; }) != 2) {
				*error = "config report lost CPU flags";
				return false;
			}
			std::cout << "[net-match-selftest] PASS cpu_roster_validation" << std::endl;
			return true;
		}

		bool TestCPURosterHash(std::string* error) {
			NetMatchConfig seated = MakeConfig();
			seated.players.push_back({0, 2, true, "CPU Alpha"});
			seated.players.push_back({0, 3, true, "CPU Beta"});
			if (!NetMatchConfigUtil::ValidateLocalAlpha(seated, error)) return false;
			const NetHash32 seatedHash = NetMatchConfigUtil::HashConfig(seated);
			NetMatchConfig reordered = seated;
			std::swap(reordered.players[2], reordered.players[3]);
			if (NetMatchConfigUtil::HashConfig(reordered) != seatedHash) {
				*error = "the CPU roster hash depends on the player vector order";
				return false;
			}
			NetMatchConfig aiOnly;
			aiOnly.sessionId = seated.sessionId;
			aiOnly.dedicated = true;
			aiOnly.peerCount = 1;
			aiOnly.players = {{0, 0, true, "CPU Alpha"}, {0, 1, true, "CPU Beta"}};
			if (!NetMatchConfigUtil::ValidateLocalAlpha(aiOnly, error)) return false;
			NetMatchConfig traded = aiOnly;
			traded.players[0].displayName = aiOnly.players[1].displayName;
			traded.players[1].displayName = aiOnly.players[0].displayName;
			if (NetMatchConfigUtil::HashConfig(traded) == NetMatchConfigUtil::HashConfig(aiOnly)) {
				*error = "trading two CPU slots between teams left the match config hash unchanged";
				return false;
			}
			NetMatchConfig moved = aiOnly;
			moved.players.back().team = 2;
			if (NetMatchConfigUtil::HashConfig(moved) == NetMatchConfigUtil::HashConfig(aiOnly)) {
				*error = "moving a CPU slot to another team left the match config hash unchanged";
				return false;
			}
			std::cout << "[net-match-selftest] PASS cpu_roster_hash" << std::endl;
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

		bool TestMatchRulesCodec(std::string* error) {
			NetMatchConfig config = MakeConfig();
			config.roundId = 7;
			config.configRevision = 13;
			config.activityModule = "Example.rte";
			config.sceneModule = "Maps.rte";
			config.difficulty = 81;
			config.startingGold = 12345;
			config.fogOfWar = config.requireClearPathToOrbit = config.deployUnits = true;
			config.autosaveEnabled = true;
			config.autosaveIntervalSeconds = 120;
			config.idleWaitMinutes = 4;
			config.automaticRepair = false;
			config.delayPolicy = NetMatchDelayPolicy::Fixed;
			config.inputDelayFrames = 2;
			config.peerInputDelayFrames = {3, 7};
			for (size_t i = 0; i < config.teamRules.size(); ++i) {
				config.teamRules[i] = {"-Random-", "Coalition.rte", static_cast<uint8_t>(40 + i)};
			}
			if (!RoundTrip(NetLobbyMatchConfig{config}, error)) return false;
			using Edit = std::pair<std::string, std::function<void(NetMatchConfig&)>>;
			const std::vector<Edit> edits = {
				{"session", [](auto& c) { ++c.sessionId; }}, {"round", [](auto& c) { ++c.roundId; }},
				{"revision", [](auto& c) { ++c.configRevision; }}, {"activity_module", [](auto& c) { c.activityModule = "Other.rte"; }},
				{"activity_class", [](auto& c) { c.activityType = "GameActivity"; }}, {"activity_preset", [](auto& c) { c.activityPreset += " 2"; }},
				{"scene_module", [](auto& c) { c.sceneModule = "Other.rte"; }}, {"scene", [](auto& c) { c.sceneName += " 2"; }},
				{"mode", [](auto& c) { c.mode = NetMatchMode::CoopPvE; }}, {"mode_preset", [](auto& c) { c.modePreset = "Co-op"; }},
				{"difficulty", [](auto& c) { ++c.difficulty; }}, {"gold", [](auto& c) { ++c.startingGold; }},
				{"fog", [](auto& c) { c.fogOfWar = false; }}, {"orbit", [](auto& c) { c.requireClearPathToOrbit = false; }},
				{"deploy", [](auto& c) { c.deployUnits = false; }}, {"autosave", [](auto& c) { c.autosaveEnabled = false; }},
				{"interval", [](auto& c) { ++c.autosaveIntervalSeconds; }}, {"idle_wait", [](auto& c) { ++c.idleWaitMinutes; }},
				{"repair", [](auto& c) { c.automaticRepair = true; }}, {"policy", [](auto& c) { c.delayPolicy = NetMatchDelayPolicy::Auto; }},
				{"floor", [](auto& c) { ++c.inputDelayFrames; }}, {"sender_1", [](auto& c) { ++c.peerInputDelayFrames[0]; }},
				{"sender_2", [](auto& c) { ++c.peerInputDelayFrames[1]; }},
			};
			const NetHash32 hash = NetMatchConfigUtil::HashConfig(config);
			NetMatchConfig arbitraryBytes = config;
			arbitraryBytes.activityModule = std::string(1, static_cast<char>(0xFF)) + ".rte";
			if (NetMatchConfigUtil::HashConfig(arbitraryBytes) == hash) { *error = "rules hash omitted module bytes"; return false; }
			auto checkEdit = [&](const Edit& edit) {
				NetMatchConfig changed = config;
				edit.second(changed);
				if (NetMatchConfigUtil::HashConfig(changed) == hash) {
					*error = "rules hash omitted " + edit.first;
					return false;
				}
				return RoundTrip(NetLobbyMatchConfig{changed}, error);
			};
			for (const auto& edit : edits) if (!checkEdit(edit)) return false;
			for (size_t i = 0; i < config.teamRules.size(); ++i) {
				if (!checkEdit({"technology intent", [i](auto& c) { c.teamRules[i].technologyIntent = "Coalition.rte"; }}) ||
				    !checkEdit({"technology module", [i](auto& c) { c.teamRules[i].technologyModule = "Dummy.rte"; }}) ||
				    !checkEdit({"AI skill", [i](auto& c) { ++c.teamRules[i].aiSkill; }})) return false;
			}
			for (const uint32_t gold : {0U, 29999U, NetMatchConfigUtil::c_InfiniteGold}) {
				config.startingGold = gold;
				if (!RoundTrip(NetLobbyMatchConfig{config}, error)) return false;
			}
			NetMatchConfig boundary = MakeConfig();
			for (const uint8_t high : {0, 1}) {
				boundary.mode = NetMatchMode::PvPvE;
				boundary.difficulty = high ? 100 : 0;
				boundary.teamRules[0].aiSkill = high ? 100 : 1;
				boundary.autosaveIntervalSeconds = std::numeric_limits<uint32_t>::max();
				boundary.inputDelayFrames = high ? 60 : 0;
				boundary.peerInputDelayFrames = {boundary.inputDelayFrames, boundary.inputDelayFrames};
				boundary.idleWaitMinutes = high ? 60 : 0;
				if (!RoundTrip(NetLobbyMatchConfig{boundary}, error)) return false;
			}
			std::cout << "[net-match-selftest] PASS rules_roundtrip" << std::endl;
			std::cout << "[net-match-selftest] PASS rules_hash_sensitivity" << std::endl;
			const std::vector<Edit> invalid = {
				{"round", [](auto& c) { c.roundId = 0; }}, {"revision", [](auto& c) { c.configRevision = 0; }},
				{"difficulty", [](auto& c) { c.difficulty = 101; }}, {"gold slider top", [](auto& c) { c.startingGold = 30000; }},
				{"gold above slider", [](auto& c) { c.startingGold = 30001; }},
				{"module path", [](auto& c) { c.activityModule = "../Base.rte"; }}, {"scene module", [](auto& c) { c.sceneModule = "Maps"; }},
				{"empty scene", [](auto& c) { c.sceneName.clear(); }}, {"empty class", [](auto& c) { c.activityType.clear(); }},
				{"module control", [](auto& c) { c.activityModule = "Bad\n.rte"; }},
				{"module bytes", [](auto& c) { c.activityModule = std::string(1, static_cast<char>(0xFF)) + ".rte"; }},
				{"module wildcard", [](auto& c) { c.sceneModule = "*.rte"; }},
				{"module length", [](auto& c) { c.sceneModule = std::string(129, 'x') + ".rte"; }},
				{"unresolved random", [](auto& c) { c.teamRules[0].technologyModule = "-Random-"; }},
				{"unresolved all", [](auto& c) { c.teamRules[0].technologyModule = "-All-"; }},
				{"technology intent", [](auto& c) { c.teamRules[0].technologyIntent = "Dummy.rte"; }},
				{"AI low", [](auto& c) { c.teamRules[0].aiSkill = 0; }}, {"AI high", [](auto& c) { c.teamRules[0].aiSkill = 101; }},
				{"autosave interval", [](auto& c) { c.autosaveIntervalSeconds = 0; }}, {"idle wait", [](auto& c) { c.idleWaitMinutes = 61; }},
				{"policy", [](auto& c) { c.delayPolicy = static_cast<NetMatchDelayPolicy>(3); }},
				{"mode", [](auto& c) { c.mode = static_cast<NetMatchMode>(4); }},
				{"delay count", [](auto& c) { c.peerInputDelayFrames.pop_back(); }},
				{"delay range", [](auto& c) { c.peerInputDelayFrames[0] = 61; }},
				{"lossy downgrade", [](auto& c) { c.version = 2; }},
			};
			for (const auto& edit : invalid) {
				NetMatchConfig changed = config;
				edit.second(changed);
				std::vector<uint8_t> bytes;
				NetLobbyError rejected;
				if (NetMatchConfigUtil::ValidateLocalAlpha(changed) || NetLobbyProtocol::Encode({NetLobbyMatchConfig{changed}}, bytes, &rejected) || rejected.message.empty()) {
					*error = "rules validation accepted " + edit.first;
					return false;
				}
			}
			std::vector<uint8_t> bytes;
			if (!NetLobbyProtocol::Encode({NetLobbyMatchConfig{config}}, bytes)) return false;
			NetMatchConfig prefix = MakeConfig();
			prefix.version = 2;
			prefix.inputDelayFrames = config.inputDelayFrames;
			prefix.peerInputDelayFrames = config.peerInputDelayFrames;
			std::vector<uint8_t> prefixBytes;
			if (!NetLobbyProtocol::Encode({NetLobbyMatchConfig{prefix}}, prefixBytes)) return false;
			const size_t difficultyOffset = prefixBytes.size() + 20 + config.activityModule.size() + config.sceneModule.size();
			for (const auto& [offset, value] : std::vector<std::pair<size_t, uint8_t>>{{difficultyOffset, 101}, {bytes.size() - 9, 0}, {bytes.size() - 3, 61}}) {
				auto invalidWire = bytes;
				invalidWire.at(offset) = value;
				const auto decoded = NetLobbyProtocol::Decode(invalidWire);
				if (decoded.ok || decoded.error.code != NetLobbyErrorCode::InvalidValue) { *error = "out-of-range rules decoded"; return false; }
			}
			for (const size_t offset : {size_t(4), size_t(NetLobbyProtocol::c_HeaderBytes)}) {
				auto newer = bytes;
				newer[offset] = 255;
				const auto decoded = NetLobbyProtocol::Decode(newer);
				if (decoded.ok || decoded.error.code != NetLobbyErrorCode::UnsupportedVersion || decoded.error.message.empty()) {
					*error = "newer rules/lobby version was not refused with a version reason";
					return false;
				}
			}
			for (size_t length = NetLobbyProtocol::c_HeaderBytes; length < bytes.size(); ++length) {
				auto truncated = std::vector<uint8_t>(bytes.begin(), bytes.begin() + length);
				for (size_t i = 0; i < 4; ++i) truncated[12 + i] = static_cast<uint8_t>((length - NetLobbyProtocol::c_HeaderBytes) >> (8 * i));
				if (NetLobbyProtocol::Decode(truncated).ok) { *error = "truncated rules decoded"; return false; }
			}
			for (const size_t offset : {bytes.size() - 8, bytes.size() - 2, bytes.size() - 1}) {
				auto invalidWire = bytes;
				invalidWire[offset] = 3;
				if (NetLobbyProtocol::Decode(invalidWire).ok) { *error = "invalid rules bool/policy decoded"; return false; }
			}
			std::cout << "[net-match-selftest] PASS rules_invalid_refused" << std::endl;
			const std::string legacyHex = "43434c340300100003000000590000000200010000000000000001020000010200000a00474153637269707465641000536b69726d69736820446566656e73650a0047726173736c616e6473030050765002010000000400486f7374020100000600436c69656e7400";
			std::vector<uint8_t> legacy;
			for (size_t i = 0; i < legacyHex.size(); i += 2) legacy.push_back(static_cast<uint8_t>(std::stoul(legacyHex.substr(i, 2), nullptr, 16)));
			NetMatchConfig expected = MakeConfig();
			expected.version = 2;
			expected.sessionId = 1;
			for (const uint8_t version : {2, 1}) {
				legacy[16] = version;
				if (version == 1) { legacy.pop_back(); --legacy[12]; }
				const auto networkDecode = NetLobbyProtocol::Decode(legacy);
				if (networkDecode.ok || networkDecode.error.code != NetLobbyErrorCode::UnsupportedVersion) {
					*error = "network decode accepted a legacy config envelope";
					return false;
				}
				const auto decoded = NetLobbyProtocol::Decode(legacy, NetLobbyDecodeOptions{true});
				const auto* payload = decoded.ok ? std::get_if<NetLobbyMatchConfig>(&decoded.message.payload) : nullptr;
				if (!payload || payload->config != expected || NetIdentity::HashHex(NetMatchConfigUtil::HashConfig(payload->config)) !=
				    "7c8b173644be7dd32f0a7c149e386e04319f836f74c3c7de64397e743e68e397") {
					*error = "legacy rules defaults or config hash changed: " + decoded.error.message;
					return false;
				}
			}
			std::cout << "[net-match-selftest] legacy_config_hash=" << NetIdentity::HashHex(NetMatchConfigUtil::HashConfig(expected)) << std::endl;
			std::cout << "[net-match-selftest] PASS rules_legacy_defaults" << std::endl;
			return true;
		}

		bool TestLobbyCodecRoundTrips(std::string* error) {
			if (!TestMatchRulesCodec(error)) return false;
			const NetMatchConfig config = MakeConfig();
			const NetHash32 configHash = NetMatchConfigUtil::HashConfig(config);
			if (!RoundTrip(NetLobbyHello{1, 1, 1, "Host", "host"}, error) ||
			    !RoundTrip(NetLobbyPeerState{2, true, 12, 2, "Client", "windows"}, error) ||
			    !RoundTrip(NetLobbyPeerState{2, false, 0, 0, "Client", "windows", false}, error) ||
			    !RoundTrip(NetLobbyMatchConfig{config}, error) ||
			    !RoundTrip(NetLobbyConfigAck{2, true, configHash, ""}, error) ||
			    !RoundTrip(NetLobbyReady{2, true}, error) ||
			    !RoundTrip(NetLobbyStart{config.sessionId, 120, 0, configHash}, error) ||
			    !RoundTrip(NetLobbyAbort{1, "user cancelled"}, error) ||
			    !RoundTrip(NetLobbySeatAssign{2}, error) ||
			    !RoundTrip(NetLobbySeatAssign{static_cast<uint8_t>(NetLobbyProtocol::c_MaxPlayers)}, error)) {
				return false;
			}
			// A seat assignment names a real seat; zero and out-of-range must not decode.
			for (const uint8_t assigned: {uint8_t{0}, static_cast<uint8_t>(NetLobbyProtocol::c_MaxPlayers + 1)}) {
				NetLobbyMessage message;
				message.payload = NetLobbySeatAssign{2};
				std::vector<uint8_t> bytes;
				NetLobbyError encodeError;
				if (!NetLobbyProtocol::Encode(message, bytes, &encodeError)) {
					*error = "could not encode a seat assignment";
					return false;
				}
				bytes.at(NetLobbyProtocol::c_HeaderBytes) = assigned;
				const NetLobbyDecodeResult decoded = NetLobbyProtocol::Decode(bytes);
				if (decoded.ok || decoded.error.code != NetLobbyErrorCode::InvalidValue) {
					*error = "seat assignment peer id " + std::to_string(assigned) + " was not rejected";
					return false;
				}
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

		bool TestLobbyStartsWithoutRemoteHumanSeats(std::string* error) {
			NetMatchServiceRequest aiOnly;
			aiOnly.host = true;
			aiOnly.dedicated = true;
			aiOnly.peerCount = 2;
			aiOnly.humans = 0;
			aiOnly.cpuSlots = 2;
			aiOnly.mode = NetMatchMode::CoopPvE;
			NetMatchConfig aiOnlyConfig;
			if (!NetMatchService::BuildMatchConfig(aiOnly, 0x4149304E4C593031ULL, aiOnlyConfig, error)) return false;

			LoopbackTransport transport;
			if (!transport.StartHost(43213, error)) return false;
			NetLobbySession lobby;
			NetLobbySessionConfig setup;
			setup.host = true;
			setup.localPeerId = 1;
			setup.matchConfig = aiOnlyConfig;
			setup.autoStart = true;
			if (!lobby.Start(transport, setup, error)) return false;
			lobby.Tick(0);
			if (!lobby.IsStarted()) {
				*error = "a dedicated host seating only CPU teams stopped at " + std::string(NetLobbySession::StateName(lobby.GetState()));
				return false;
			}

			// A roster that seats a human on another peer still waits for that peer.
			LoopbackTransport waitingTransport;
			if (!waitingTransport.StartHost(43214, error)) return false;
			NetLobbySession waiting;
			NetLobbySessionConfig waitingSetup;
			waitingSetup.host = true;
			waitingSetup.localPeerId = 1;
			waitingSetup.matchConfig = MakeConfig();
			waitingSetup.autoStart = true;
			if (!waiting.Start(waitingTransport, waitingSetup, error)) return false;
			waiting.Tick(0);
			if (waiting.IsStarted() || waiting.GetState() != NetLobbyState::WaitingForConfigAck) {
				*error = "a roster with a remote human seat started at " + std::string(NetLobbySession::StateName(waiting.GetState()));
				return false;
			}
			std::cout << "[net-match-selftest] PASS ai_only_lobby_starts" << std::endl;
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
			std::cout << "[net-match-selftest] PASS healed round planned end: host_total=" << hostAtStopFrame
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
			if (!NetMatchRunner::RematchRostersAgree(derived, derived, 2) || NetMatchRunner::RematchRostersAgree(four, derived, 2)) {
				*error = "the rematch proposal check accepted a roster the peer did not derive";
				return false;
			}
			NetMatchConfig intact;
			if (!NetMatchConfigUtil::DeriveRematchConfig(four, {1, 2, 3, 4}, intact, nullptr, error)) {
				return false;
			}
			if (NetMatchConfigUtil::HashConfig(intact) != NetMatchConfigUtil::HashConfig(four) || !NetMatchRunner::RematchRostersAgree(intact, four, 2)) {
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

		// Setup rounds on worker threads, one per peer, as each process's match worker runs its own.
		class RematchWorkers {
		public:
			~RematchWorkers() {
				for (std::thread& thread: m_Threads) {
					if (thread.joinable()) thread.join();
				}
			}

			void Add(RematchPeer& peer, std::function<bool(RematchPeer&, std::string*)> setup) {
				peer.round = std::make_unique<NetLockstepCoordinator>();
				peer.handover.clear();
				peer.seats.reset();
				peer.trace.clear();
				m_Peers.push_back(&peer);
				m_Launched.push_back(0);
				std::string* error = m_Errors.emplace_back(std::make_unique<std::string>()).get();
				std::atomic<int>* outcome = m_Outcomes.emplace_back(std::make_unique<std::atomic<int>>(0)).get();
				m_Threads.emplace_back([&peer, error, outcome, setup = std::move(setup)] { outcome->store(setup(peer, error) ? 1 : 2); });
				for (int spin = 0; peer.host && spin < 2000 && !peer.transport.IsHosting(); ++spin) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			}

			/// The peer's setup error once its setup has failed; empty while it runs or once it started.
			std::string FailureOf(const RematchPeer& peer) const {
				for (size_t index = 0; index < m_Peers.size(); ++index) {
					if (m_Peers[index] == &peer && m_Outcomes[index]->load() == 2) return *m_Errors[index];
				}
				return {};
			}

			// A settled peer launches at once and keeps relaying the starts the others wait on, as the sim does.
			bool Settle(std::string* error) {
				for (bool settingUp = true; settingUp;) {
					settingUp = false;
					for (size_t index = 0; index < m_Peers.size(); ++index) {
						const int state = m_Outcomes[index]->load();
						if (state == 0) {
							settingUp = true;
						} else if (state == 1) {
							if (!m_Launched[index]) {
								AttachRematchRound(*m_Peers[index]);
								m_Launched[index] = 1;
							}
							m_Peers[index]->round->Tick(NetLockstepNowMs());
						}
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				for (std::thread& thread: m_Threads) {
					if (thread.joinable()) thread.join();
				}
				std::string failures;
				for (size_t index = 0; index < m_Peers.size(); ++index) {
					if (m_Outcomes[index]->load() != 1) {
						failures += (failures.empty() ? "" : "; ") + m_Peers[index]->name + ": " + *m_Errors[index];
					}
				}
				if (!failures.empty()) {
					*error = failures;
				}
				return failures.empty();
			}

		private:
			std::vector<RematchPeer*> m_Peers;
			std::vector<char> m_Launched;
			std::vector<std::unique_ptr<std::string>> m_Errors;
			std::vector<std::unique_ptr<std::atomic<int>>> m_Outcomes; //!< 0 setting up, 1 started, 2 failed.
			std::vector<std::thread> m_Threads;
		};

		bool LaunchRematchRound(const std::vector<RematchPeer*>& peers, const std::function<bool(RematchPeer&, std::string*)>& setup, std::string* error) {
			RematchWorkers workers;
			for (RematchPeer* peer: peers) {
				workers.Add(*peer, setup);
			}
			return workers.Settle(error);
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

		bool StartRematchPeer(RematchPeer& peer, std::string* setupError) {
			return peer.runner.Start(peer.transport, peer.session, *peer.round, peer.config, setupError);
		}

		// Both survivors play the host's two-seat roster, on one config, with the survivor on the given id.
		bool CheckRematchRelaunch(RematchFixture& fixture, RematchPeer& survivor, uint8_t survivorId, std::string& details, std::string* error) {
			RematchPeer& host = fixture.Host();
			const NetMatchConfig& roster = host.runner.GetMatchConfig();
			std::vector<uint8_t> seated;
			for (const NetMatchPlayerSlot& slot: roster.players) {
				if (!slot.cpu) seated.push_back(slot.peerId);
			}
			std::sort(seated.begin(), seated.end());
			if (roster.peerCount != 2 || seated != std::vector<uint8_t>{1, 2}) {
				*error = "the host did not propose the two survivors: peer_count " + std::to_string(roster.peerCount);
				return false;
			}
			const NetHash32 hash = NetMatchConfigUtil::HashConfig(roster);
			if (NetMatchConfigUtil::HashConfig(survivor.runner.GetMatchConfig()) != hash) {
				*error = "the survivor relaunched on another roster than the host's";
				return false;
			}
			const std::vector<NetSessionPeerInfo> ready = host.session.GetReadyPeers();
			if (survivor.LockstepId() != survivorId || survivor.round->GetConfig().localPeerId != survivorId || ready.size() != 1 ||
			    ready.front().assignedPeerId + 1 != survivorId || host.round->GetConfig().peerCount != 2 || survivor.round->GetConfig().peerCount != 2) {
				*error = "the survivor's id is " + std::to_string(survivor.LockstepId()) + " where the host seats lockstep peer " +
				         (ready.size() == 1 ? std::to_string(ready.front().assignedPeerId + 1) : std::string("none"));
				return false;
			}
			if (!PlayRematchTicks(fixture, 6)) {
				*error = "the rematch round never committed";
				return false;
			}
			size_t shared = 0;
			for (const auto& [frame, text]: host.trace) {
				const auto other = survivor.trace.find(frame);
				if (other == survivor.trace.end()) continue;
				if (other->second != text) {
					*error = "the rematch round diverged at tick " + std::to_string(frame) + ": host '" + text + "' survivor '" + other->second + "'";
					return false;
				}
				++shared;
			}
			details = "peer_count=2 survivor_lockstep=" + std::to_string(survivorId) + " config_hash=" + NetIdentity::HashHex(hash).substr(0, 16) +
			          " shared_ticks=" + std::to_string(shared);
			return shared >= 6;
		}

		// The middle seat's process dies inside round 1 and nobody reclaims it: its seat's hold runs out.
		bool RematchAfterDropInRound(std::string& details, std::string* error) {
			RematchFixture fixture;
			if (!SetUpRematchFixture(fixture, "drop-in-round", 43160, 3, error)) return false;
			std::string step;
			auto fail = [&](const std::string& what) {
				*error = "rematch after a hard drop in round 1: " + what + (step.empty() ? "" : ": " + step);
				StopRematchFixture(fixture);
				return false;
			};
			if (!LaunchRematchRound(LiveRematchPeers(fixture), &StartRematchPeer, &step)) return fail("round 1 setup failed");
			if (!PlayRematchTicks(fixture, 6)) return fail("round 1 never committed");
			RematchPeer& host = fixture.Host();
			RematchPeer* dropped = fixture.Client(2);
			RematchPeer* survivor = fixture.Client(3);
			if (!dropped || !survivor) return fail("round 1 did not seat lockstep peers 2 and 3");
			dropped->gone = true;
			dropped->transport.Stop();
			if (!PumpRematchUntil(fixture, 3000, [&] {
				    return host.round->GetPeerLeaveFrames().contains(2) && survivor->round->GetPeerLeaveFrames().contains(2) && host.admission.GetStats().seatsDropped == 1;
			    })) {
				return fail("the drop never reached both rounds and the host's plane");
			}
			fixture.clock.skippedMs += NetReconnectHost::c_ProvisionalExpiryMs + 1000;
			if (!PumpRematchUntil(fixture, 3000, [&] {
				    return host.round->HeldSeatResolution(2) == NetLockstepHoldResolution::Expired && survivor->round->HeldSeatResolution(2) == NetLockstepHoldResolution::Expired;
			    })) {
				std::string seats;
				for (const NetH4SeatStatus& status: host.admission.GetSeatStatuses()) {
					seats += " seat" + std::to_string(status.stableSeat) + "/peer" + std::to_string(status.lockstepPeerId) + (status.committed ? " committed" : "") +
					         (status.dropped ? " dropped" : "") + (status.closed ? " closed" : "");
				}
				return fail("the dropped seat's hold was never resolved as expired (host round " + std::string(NetLockstepCoordinator::StateName(host.round->GetState())) +
				            " resolution " + std::to_string(static_cast<int>(host.round->HeldSeatResolution(2))) + ", survivor round " +
				            NetLockstepCoordinator::StateName(survivor->round->GetState()) + " resolution " + std::to_string(static_cast<int>(survivor->round->HeldSeatResolution(2))) +
				            " " + survivor->round->GetStats().timeoutReason + "; plane dropped=" + std::to_string(host.admission.GetStats().seatsDropped) +
				            " holds_expired=" + std::to_string(host.admission.GetStats().seatHoldsExpired) + seats + ")");
			}
			if (!PlayRematchTicks(fixture, 4) || !FinishRematchRound(fixture, &step)) return fail("round 1 did not play on and finish");
			if (!RematchFixtureRound(fixture, &step)) return fail("the rematch did not relaunch");
			if (!CheckRematchRelaunch(fixture, *survivor, 2, details, &step)) return fail("the rematch roster");
			StopRematchFixture(fixture);
			return true;
		}

		// The last seat's process dies after round 1 ends: only the host's transport can know it is gone.
		bool RematchAfterDropBetweenRounds(std::string& details, std::string* error) {
			RematchFixture fixture;
			if (!SetUpRematchFixture(fixture, "drop-after-round", 43162, 3, error)) return false;
			std::string step;
			auto fail = [&](const std::string& what) {
				*error = "rematch after a hard drop between rounds: " + what + (step.empty() ? "" : ": " + step);
				StopRematchFixture(fixture);
				return false;
			};
			if (!LaunchRematchRound(LiveRematchPeers(fixture), &StartRematchPeer, &step)) return fail("round 1 setup failed");
			if (!PlayRematchTicks(fixture, 6) || !FinishRematchRound(fixture, &step)) return fail("round 1 did not finish");
			RematchPeer* survivor = fixture.Client(2);
			RematchPeer* dropped = fixture.Client(3);
			if (!survivor || !dropped) return fail("round 1 did not seat lockstep peers 2 and 3");
			dropped->gone = true;
			dropped->transport.Stop();
			if (ClientRematchSurvivors(*survivor) != std::vector<uint8_t>{1, 2, 3}) return fail("the survivor's round saw the drop after all");
			if (!RematchFixtureRound(fixture, &step)) return fail("the rematch did not relaunch");
			if (!CheckRematchRelaunch(fixture, *survivor, 2, details, &step)) return fail("the rematch roster");
			StopRematchFixture(fixture);
			return true;
		}

		// The MIDDLE seat's process dies after round 1 ends, so the survivor below it moves up a seat.
		bool RematchAfterMiddleDropBetweenRounds(std::string& details, std::string* error) {
			RematchFixture fixture;
			if (!SetUpRematchFixture(fixture, "middle-drop-after-round", 43164, 3, error)) return false;
			std::string step;
			auto fail = [&](const std::string& what) {
				*error = "rematch after a middle-seat drop between rounds: " + what + (step.empty() ? "" : ": " + step);
				StopRematchFixture(fixture);
				return false;
			};
			if (!LaunchRematchRound(LiveRematchPeers(fixture), &StartRematchPeer, &step)) return fail("round 1 setup failed");
			if (!PlayRematchTicks(fixture, 6) || !FinishRematchRound(fixture, &step)) return fail("round 1 did not finish");
			RematchPeer* dropped = fixture.Client(2);
			RematchPeer* survivor = fixture.Client(3);
			if (!survivor || !dropped) return fail("round 1 did not seat lockstep peers 2 and 3");
			dropped->gone = true;
			dropped->transport.Stop();
			if (ClientRematchSurvivors(*survivor) != std::vector<uint8_t>{1, 2, 3}) return fail("the survivor's round saw the drop after all");
			if (!RematchFixtureRound(fixture, &step)) return fail("the rematch did not relaunch");
			if (!CheckRematchRelaunch(fixture, *survivor, 2, details, &step)) return fail("the rematch roster");
			details += " survivor_derived_lockstep=3 adopted_lockstep=" + std::to_string(survivor->LockstepId());
			StopRematchFixture(fixture);
			return true;
		}

		// Three peers, one hard drop nobody reclaims, then a rematch: the host proposes the two survivors.
		bool TestRematchAfterAHardDrop(std::string* error) {
			std::string inRound;
			std::string betweenRounds;
			std::string middleSeat;
			std::string details;
			if (RematchAfterDropInRound(details, &inRound)) {
				std::cout << "PASS rematch_after_hard_drop in_round " << details << std::endl;
			}
			if (RematchAfterDropBetweenRounds(details, &betweenRounds)) {
				std::cout << "PASS rematch_after_hard_drop between_rounds " << details << std::endl;
			}
			if (RematchAfterMiddleDropBetweenRounds(details, &middleSeat)) {
				std::cout << "PASS rematch_after_hard_drop middle_seat_between_rounds " << details << std::endl;
			}
			for (const std::string* failure: {&inRound, &betweenRounds, &middleSeat}) {
				if (!failure->empty()) *error += (error->empty() ? "" : "; ") + *failure;
			}
			return error->empty();
		}

		// A survivor that moved up in the rematch drops inside the next round and reclaims its stable seat.
		bool ReclaimAfterShrinks(uint16_t port, int shrinks, bool waitForReturnerLobby, std::string& details, std::string* error) {
			RematchFixture fixture;
			if (!SetUpRematchFixture(fixture, "reclaim-after-" + std::to_string(shrinks) + "-shrinks" + (waitForReturnerLobby ? "" : "-nowait"), port, 4, error)) return false;
			std::unique_ptr<RematchPeer> returner;
			RematchWorkers workers;
			std::string step;
			const std::string round = "round " + std::to_string(shrinks + 1);
			auto fail = [&](const std::string& what) {
				*error = "rematch " + round + " reclaim after " + std::to_string(shrinks) + " shrink(s)" + (waitForReturnerLobby ? "" : " without the lobby wait") +
				         ": " + what + (step.empty() ? "" : ": " + step);
				StopRematchFixture(fixture);
				if (returner) returner->transport.Stop();
				return false;
			};
			if (!LaunchRematchRound(LiveRematchPeers(fixture), &StartRematchPeer, &step)) return fail("round 1 setup failed");
			if (!PlayRematchTicks(fixture, 6)) return fail("round 1 never committed");
			RematchPeer* mover = fixture.Client(4);
			if (!mover) return fail("round 1 did not seat lockstep peer 4");
			const uint16_t stableSeat = mover->reconnect.GetRecord().stableSeat;
			for (int shrink = 1; shrink <= shrinks; ++shrink) {
				RematchPeer* leaver = fixture.Client(shrink == 1 ? 3 : 2);
				if (!leaver || !LeaveRematchRound(fixture, *leaver, &step) || !PlayRematchTicks(fixture, 2) || !FinishRematchRound(fixture, &step)) {
					return fail("clean leave " + std::to_string(shrink) + " did not settle");
				}
				if (!RematchFixtureRound(fixture, &step)) return fail("rematch " + std::to_string(shrink) + " did not relaunch");
			}
			if (!PlayRematchTicks(fixture, 6)) return fail(round + " never committed");
			RematchPeer& host = fixture.Host();
			const uint8_t moverId = mover->LockstepId();
			const NetReconnectHostStats planeBefore = host.admission.GetStats();
			NetPeerId holder = c_InvalidNetPeerId;
			uint32_t generation = 0;
			uint32_t incarnationBefore = 0;
			(void)host.admission.GetSeatHolder(stableSeat, holder, generation, incarnationBefore);
			mover->gone = true;
			mover->transport.Stop();
			if (!PumpRematchUntil(fixture, 3000, [&] {
				    const auto live = LiveRematchPeers(fixture);
				    return std::all_of(live.begin(), live.end(), [moverId](RematchPeer* peer) { return peer->round->GetPeerLeaveFrames().contains(moverId); });
			    })) {
				return fail("the drop never reached every round");
			}
			const uint64_t dropFrame = host.round->GetPeerLeaveFrames().at(moverId);
			if (!PumpRematchUntil(fixture, 3000, [&] { return host.admission.GetStats().seatsDropped == planeBefore.seatsDropped + 1; })) {
				return fail("the host's plane recorded no dropped seat for lockstep peer " + std::to_string(moverId));
			}
			// The same player comes back on a new connection, with the ticket its round-1 seat gave it.
			returner = std::make_unique<RematchPeer>();
			returner->name = "returner";
			returner->config = mover->config;
			returner->config.sessionConfig.localNonce += 100;
			returner->config.lobbyWaitMs = 8000;
			auto lobbyUp = std::make_shared<std::atomic<bool>>(false);
			returner->config.publishLobby = [lobbyUp](const NetLobbySnapshot& snapshot) {
				if (snapshot.lobbyPhase == NetMatchRunner::StateName(NetMatchRuntimeState::LobbySync)) lobbyUp->store(true);
			};
			returner->store.SetPath(mover->store.GetPath());
			returner->reconnect.Configure(&returner->store, RematchIdentity(), "Returner");
			returner->reconnect.SetUnixClock(&RematchUnixClock, nullptr);
			returner->reconnect.SetHostContext("loopback", NetHash32{});
			returner->session.SetReconnectClient(&returner->reconnect);
			RematchPeer* rejoined = returner.get();
			workers.Add(*rejoined, &StartRematchPeer);
			const bool reclaimed = PumpRematchUntil(fixture, 5000, [&] {
				return !workers.FailureOf(*rejoined).empty() || host.round->HeldSeatResolution(moverId) == NetLockstepHoldResolution::Reclaimed;
			});
			if (!reclaimed || host.round->HeldSeatResolution(moverId) != NetLockstepHoldResolution::Reclaimed) {
				return fail("the returner was not admitted on stable seat " + std::to_string(stableSeat) + " (reclaims_accepted=" +
				            std::to_string(host.admission.GetStats().reclaimsAccepted) + "): " + workers.FailureOf(*rejoined));
			}
			if (!PumpRematchUntil(fixture, 3000, [&] {
				    const auto live = LiveRematchPeers(fixture);
				    return std::none_of(live.begin(), live.end(), [](RematchPeer* peer) { return peer->round->IsRunning(); });
			    })) {
				return fail("the reclaim never ended " + round + " for its heal");
			}
			std::map<RematchPeer*, std::map<uint64_t, std::string>> played{{mover, mover->trace}};
			for (RematchPeer* peer: LiveRematchPeers(fixture)) {
				if (peer->round->GetStats().timeoutReason.find("ResyncRequested") == std::string::npos) {
					return fail(peer->name + "'s round ended with '" + peer->round->GetStats().timeoutReason + "', not a resync");
				}
				played[peer] = peer->trace;
			}
			// A real heal saves first, which gives the returner time to reach its lobby before the snapshot streams.
			if (waitForReturnerLobby && (!PumpRematchUntil(fixture, 3000, [&] { return lobbyUp->load() || !workers.FailureOf(*rejoined).empty(); }) || !lobbyUp->load())) {
				return fail("the returner never reached its lobby: " + workers.FailureOf(*rejoined));
			}
			const bool lobbyUpAtHeal = lobbyUp->load();
			// ResyncMatch: resume where the host's sim stopped; each session first gets what its round held.
			const uint64_t resumeFrame = host.round->GetResumeFrame();
			host.runner.SetStartFrame(ScenarioRunner::ResyncResumeStartFrame(resumeFrame));
			std::vector<uint8_t> snapshot(9000);
			for (size_t index = 0; index < snapshot.size(); ++index) {
				snapshot[index] = static_cast<uint8_t>(index * 131U);
			}
			for (RematchPeer* peer: LiveRematchPeers(fixture)) {
				std::vector<NetTransportEvent> events;
				events.swap(peer->handover);
				for (const NetTransportEvent& event: events) {
					peer->session.InjectEvent(event, fixture.clock.NowMs());
				}
				if (peer->host) {
					workers.Add(*peer, [snapshot](RematchPeer& healer, std::string* setupError) {
						return healer.runner.StartNextMatch(healer.transport, healer.session, *healer.round, setupError, snapshot);
					});
				} else {
					workers.Add(*peer, [](RematchPeer& healer, std::string* setupError) {
						return healer.runner.StartNextMatch(healer.transport, healer.session, *healer.round, setupError);
					});
				}
			}
			if (!workers.Settle(&step)) {
				std::vector<RematchPeer*> healers = LiveRematchPeers(fixture);
				healers.push_back(rejoined);
				for (RematchPeer* peer: healers) {
					const NetLockstepConfig& config = peer->round->GetConfig();
					step += "; " + peer->name + " lockstep=" + std::to_string(config.localPeerId) + " start=" + std::to_string(config.startFrame) +
					        " resume=" + std::to_string(config.resumeFromSnapshot) + " peers=" + std::to_string(config.peerCount) +
					        " received=" + std::to_string(peer->runner.TakeReceivedState().size()) +
					        " session_ignored_phase_packets=" + std::to_string(peer->session.GetStats().ignoredPhasePackets);
				}
				return fail("the healed round did not relaunch");
			}
			fixture.peers.push_back(std::move(returner));
			uint32_t incarnation = 0;
			NetPeerId returnerConnection = c_InvalidNetPeerId;
			for (const NetSessionPeerInfo& ready: host.session.GetReadyPeers()) {
				if (ready.assignedPeerId + 1 == rejoined->LockstepId()) returnerConnection = ready.transportPeerId;
			}
			const bool seatHeld = host.admission.GetSeatHolder(stableSeat, holder, generation, incarnation);
			if (!rejoined->reconnect.UsedStoredTicket() || rejoined->reconnect.GetState() != NetH4ClientState::Joined ||
			    rejoined->reconnect.GetRecord().stableSeat != stableSeat || rejoined->LockstepId() != moverId || !seatHeld || holder != returnerConnection ||
			    incarnation != incarnationBefore + 1 || generation != rejoined->reconnect.GetRecord().holderGeneration ||
			    host.admission.GetStats().reclaimsAccepted != planeBefore.reclaimsAccepted + 1) {
				return fail("the returner is lockstep peer " + std::to_string(rejoined->LockstepId()) + " where stable seat " + std::to_string(stableSeat) +
				            " holds incarnation " + std::to_string(incarnation) + " of holder generation " + std::to_string(generation));
			}
			size_t returnerReceived = 0;
			for (RematchPeer* peer: LiveRematchPeers(fixture)) {
				if (!peer->round->GetConfig().resumeFromSnapshot || peer->round->GetConfig().startFrame != resumeFrame) {
					return fail(peer->name + " did not resume at frame " + std::to_string(resumeFrame));
				}
				if (peer->host) continue;
				const std::vector<uint8_t> received = peer->runner.TakeReceivedState();
				if (peer == rejoined) returnerReceived = received.size();
				if (received != snapshot) {
					return fail(peer->name + " did not receive the host's snapshot (" + std::to_string(received.size()) + " of " + std::to_string(snapshot.size()) + " bytes)");
				}
			}
			if (!PlayRematchTicks(fixture, 8)) return fail("the healed round never committed");
			const auto sameTicks = [&step](const std::map<uint64_t, std::string>& reference, const std::map<uint64_t, std::string>& other, const std::string& who, size_t& shared) {
				shared = 0;
				for (const auto& [frame, text]: reference) {
					const auto found = other.find(frame);
					if (found == other.end()) continue;
					if (found->second != text) {
						step = who + " diverged at tick " + std::to_string(frame) + ": '" + text + "' vs '" + found->second + "'";
						return false;
					}
					++shared;
				}
				return true;
			};
			size_t shared = 0;
			size_t before = std::numeric_limits<size_t>::max();
			for (const auto& [peer, trace]: played) {
				if (peer == &host) continue;
				if (!sameTicks(played.at(&host), trace, peer->name, shared)) return fail(round + " before the drop");
				before = std::min(before, shared);
			}
			size_t healed = std::numeric_limits<size_t>::max();
			for (RematchPeer* peer: LiveRematchPeers(fixture)) {
				if (peer->host) continue;
				if (!sameTicks(host.trace, peer->trace, peer->name, shared)) return fail("the healed " + round);
				healed = std::min(healed, shared);
			}
			if (healed < 8 || before == 0) return fail("the rounds shared too few ticks to compare");
			details = "stable_seat=" + std::to_string(stableSeat) + " lockstep=" + std::to_string(moverId) + " (round-1 lockstep 4) incarnation=" +
			          std::to_string(incarnationBefore) + "->" + std::to_string(incarnation) + " holder_generation=" + std::to_string(generation) + " used_stored_ticket=1 drop_frame=" +
			          std::to_string(dropFrame) + " heal_start=" + std::to_string(resumeFrame) + " ticks_identical_before_drop=" + std::to_string(before) +
			          " healed_ticks_identical=" + std::to_string(healed) + " peers=" + std::to_string(LiveRematchPeers(fixture).size()) +
			          " lobby_up_at_heal=" + std::to_string(lobbyUpAtHeal ? 1 : 0) + " returner_received=" + std::to_string(returnerReceived);
			StopRematchFixture(fixture);
			return true;
		}

		// After a shrink and its rematch, a drop and a reclaim inside the next round, healed on every peer alike.
		bool TestRematchRoundReclaimsAfterAShrink(std::string* error) {
			std::string control;
			std::string oneShrink;
			std::string twoShrinks;
			std::string lateLobby;
			std::string details;
			// The same reclaim inside round 1, where no rematch has re-formed anything yet.
			if (ReclaimAfterShrinks(43168, 0, true, details, &control)) {
				std::cout << "PASS rematch_round_reclaim round=1 shrinks=0 " << details << std::endl;
			}
			if (ReclaimAfterShrinks(43170, 1, true, details, &oneShrink)) {
				std::cout << "PASS rematch_round_reclaim round=2 shrinks=1 " << details << std::endl;
			}
			if (ReclaimAfterShrinks(43172, 2, true, details, &twoShrinks)) {
				std::cout << "PASS rematch_round_reclaim round=3 shrinks=2 " << details << std::endl;
			}
			// The host heals the moment the reclaim lands, so the returner's lobby comes up after the chunk.
			if (ReclaimAfterShrinks(43174, 0, false, details, &lateLobby)) {
				std::cout << "PASS rematch_round_reclaim round=1 shrinks=0 late_returner_lobby " << details << std::endl;
			}
			for (const std::string* failure: {&control, &oneShrink, &twoShrinks, &lateLobby}) {
				if (!failure->empty()) *error += (error->empty() ? "" : "; ") + *failure;
			}
			return error->empty();
		}

		// A host that knows of more drops is followed; a proposal that adds, reorders, re-teams or unseats is refused by name.
		bool TestRematchProposalFits(std::string* error) {
			const auto roster = [](std::vector<std::pair<uint8_t, uint8_t>> seats) {
				NetMatchConfig config = MakeConfig();
				config.peerCount = static_cast<uint8_t>(seats.size());
				config.players.clear();
				for (const auto& [peerId, team]: seats) {
					config.players.push_back(NetMatchPlayerSlot{peerId, team, false, "Player " + std::to_string(peerId)});
				}
				return config;
			};
			const NetMatchConfig four = roster({{1, 0}, {2, 1}, {3, 2}, {4, 3}});
			const NetMatchConfig three = roster({{1, 0}, {2, 1}, {3, 2}});
			NetMatchConfig otherHub = four;
			otherHub.hostPeerId = 2;
			struct Proposal {
				const char* name;
				NetMatchConfig proposed;
				NetMatchConfig derived;
				uint8_t local;
				const char* reason; //!< Empty when the proposal must be accepted.
			};
			const std::vector<Proposal> proposals = {
			    {"the roster it derived", four, four, 2, ""},
			    {"a roster less a seat after this peer's", three, four, 3, ""},
			    {"a roster less two seats after this peer's", roster({{1, 0}, {2, 1}}), four, 2, ""},
			    {"a proposal that adds a seat", four, three, 3, "the host proposed a seat this peer did not derive"},
			    {"a proposal that reorders", roster({{1, 0}, {2, 2}, {3, 1}, {4, 3}}), four, 4, "the host proposed the seats in another order"},
			    {"a proposal that changes a team", roster({{1, 0}, {2, 1}, {3, 0}, {4, 3}}), four, 4, "the host proposed a seat on another team"},
			    {"a proposal that changes this peer's team", roster({{1, 0}, {2, 1}, {3, 2}, {4, 1}}), four, 4, "the host proposed this peer on another team"},
			    {"a proposal that drops this peer", three, four, 4, "the host proposed a roster without this peer"},
			    {"a proposal that names another hub", otherHub, four, 2, "the host proposed a different hub"},
			};
			for (const Proposal& proposal: proposals) {
				std::string reason;
				const bool accepted = NetMatchRunner::RematchRostersAgree(proposal.proposed, proposal.derived, proposal.local, &reason);
				if (accepted != (proposal.reason[0] == '\0') || (!accepted && reason != proposal.reason)) {
					*error = std::string("rematch proposal check: ") + proposal.name + (accepted ? " was accepted" : " was refused with '" + reason + "'");
					return false;
				}
			}
			const auto seatView = [](const NetMatchConfig& played, uint8_t peerId, NetSeatPresenceState state) {
				NetLockstepSeatSnapshot view;
				for (const NetMatchPlayerSlot& slot: played.players) {
					NetSeatPresenceEntry entry;
					entry.stableSeat = static_cast<uint16_t>(slot.peerId - 1);
					entry.peerId = slot.peerId;
					entry.state = slot.peerId == peerId ? state : NetSeatPresenceState::Present;
					view.seats.push_back(entry);
				}
				return view;
			};
			NetMatchConfig withCpu = four;
			withCpu.players.push_back(NetMatchPlayerSlot{0, 3, true, "CPU"});
			const NetLockstepSeatSnapshot allPresent = seatView(four, 0, NetSeatPresenceState::Present);
			const NetLockstepSeatSnapshot fourDropped = seatView(four, 4, NetSeatPresenceState::Disconnected);
			const NetLockstepSeatSnapshot twoLeft = seatView(four, 2, NetSeatPresenceState::Left);
			const NetLockstepSeatSnapshot threeReconnecting = seatView(four, 3, NetSeatPresenceState::Reconnecting);
			const NetLockstepSeatSnapshot threeSubstituted = seatView(four, 3, NetSeatPresenceState::Substituted);
			const NetLockstepSeatSnapshot inRoundDrop = seatView(three, 2, NetSeatPresenceState::Disconnected);
			const NetLockstepSeatSnapshot unobserved = seatView(three, 0, NetSeatPresenceState::Present);
			struct Survivors {
				const char* name;
				std::vector<uint8_t> derived;
				std::vector<uint8_t> expected;
			};
			const std::vector<Survivors> survivorCases = {
			    {"a leave the round saw", NetMatchRunner::DeriveRematchSurvivors(four, {{3, 40}}, {}, nullptr), {1, 2, 4}},
			    {"a dropped seat that was reclaimed", NetMatchRunner::DeriveRematchSurvivors(four, {{3, 40}}, {3}, &allPresent), {1, 2, 3, 4}},
			    {"a drop only the host's seat view shows", NetMatchRunner::DeriveRematchSurvivors(four, {}, {}, &fourDropped), {1, 2, 3}},
			    {"a seat the host shows closed", NetMatchRunner::DeriveRematchSurvivors(four, {}, {}, &twoLeft), {1, 3, 4}},
			    {"a reclaim still in flight", NetMatchRunner::DeriveRematchSurvivors(four, {}, {}, &threeReconnecting), {1, 2, 4}},
			    {"a substituted seat", NetMatchRunner::DeriveRematchSurvivors(four, {}, {}, &threeSubstituted), {1, 2, 3, 4}},
			    {"a cpu slot", NetMatchRunner::DeriveRematchSurvivors(withCpu, {}, {}, nullptr), {1, 2, 3, 4}},
			    {"the in-round drop case", NetMatchRunner::DeriveRematchSurvivors(three, {{2, 7}}, {}, &inRoundDrop), {1, 3}},
			    {"the between-rounds drop case", NetMatchRunner::DeriveRematchSurvivors(three, {}, {}, &unobserved), {1, 2, 3}},
			};
			for (const Survivors& survivors: survivorCases) {
				if (survivors.derived != survivors.expected) {
					*error = std::string("rematch survivor derivation: ") + survivors.name + " derived " + std::to_string(survivors.derived.size()) + " survivors";
					return false;
				}
			}
			std::cout << "PASS rematch_proposal_fits proposals=" << proposals.size() << " survivor_cases=" << survivorCases.size() << std::endl;
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

	bool TestChatRoutingAndBounds(std::string* error) {
		// A host plus one teammate and one opponent of the sending client, so a team line has both
		// an eligible relay target (the host sinks what it relays) and an ineligible one.
		const uint16_t port = 43260;
		LoopbackTransport hostTransport, teamTransport, otherTransport;
		NetSession host, teamPeer, otherPeer;
		NetSessionConfig hostConfig;
		hostConfig.port = port;
		hostConfig.displayName = "Host";
		hostConfig.maxPeers = 2;
		hostConfig.heartbeatIntervalMs = 25;
		hostConfig.timeoutMs = 120000;
		NetIdentityManifest& identity = hostConfig.localIdentity;
		identity.gameVersion = "7.0.0-test";
		identity.networkProtocolVersion = NetProtocol::c_Version;
		identity.controllerFrameVersion = ControllerFrame::c_Version;
		identity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
		identity.buildId = "chat-routing-selftest";
		identity.platform = "test";
		NetSessionConfig teamConfig = hostConfig;
		teamConfig.displayName = "Mate";
		++teamConfig.localNonce;
		NetSessionConfig otherConfig = hostConfig;
		otherConfig.displayName = "Foe";
		otherConfig.localNonce += 2;
		if (!host.StartHost(hostTransport, hostConfig, error) ||
		    !teamPeer.StartClient(teamTransport, "loopback", teamConfig, error) ||
		    !otherPeer.StartClient(otherTransport, "loopback", otherConfig, error)) {
			return false;
		}
		const auto pump = [&](uint64_t steps) {
			for (uint64_t step = 0; step < steps; ++step) {
				hostTransport.AdvanceTimeMs(10);
				teamTransport.AdvanceTimeMs(10);
				otherTransport.AdvanceTimeMs(10);
				host.Tick(hostTransport.NowMs());
				teamPeer.Tick(teamTransport.NowMs());
				otherPeer.Tick(otherTransport.NowMs());
			}
		};
		for (uint64_t waited = 0; waited <= 4000 &&
		     !(host.GetReadyPeerCount() == 2 && teamPeer.IsReady() && otherPeer.IsReady()); waited += 10) {
			pump(1);
		}
		if (host.GetReadyPeerCount() != 2 || !teamPeer.IsReady() || !otherPeer.IsReady()) {
			*error = "the chat fixture never reached Ready on every session";
			return false;
		}
		const uint8_t teamId = teamPeer.GetLocalPeerId();
		const uint8_t otherId = otherPeer.GetLocalPeerId();
		if (teamId == 0 || otherId == 0 || teamId == otherId) {
			*error = "the clients did not receive distinct session ids";
			return false;
		}
		const std::map<uint8_t, int> teams{{0, 0}, {teamId, 0}, {otherId, 1}};
		host.SetChatTeams(teams);
		teamPeer.SetChatTeams(teams);
		otherPeer.SetChatTeams(teams);
		const auto lastLines = [](NetSession& session) {
			return session.TakeChatEntries();
		};
		const auto hasLine = [](const std::vector<NetChatEntry>& entries, uint8_t sender, uint8_t scope,
		                        const std::string& text) {
			for (const NetChatEntry& entry: entries) {
				if (entry.senderPeerId == sender && entry.scope == scope && entry.text == text) return true;
			}
			return false;
		};
		// Every arm reports independently so one run against a defective build shows each defect.
		std::vector<std::string> fails;
		const auto fail = [&](const std::string& what) { fails.push_back(what); };

		// All-scope: the host sinks the client's line and relays it to the other peer too.
		if (!teamPeer.SendChat(c_NetChatScopeAll, "covering left")) {
			fail("an in-limit all-scope send was refused locally");
		}
		pump(4);
		const std::vector<NetChatEntry> hostLog = lastLines(host);
		const std::vector<NetChatEntry> otherLog = lastLines(otherPeer);
		const std::vector<NetChatEntry> teamEcho = lastLines(teamPeer);
		if (!hasLine(hostLog, teamId, c_NetChatScopeAll, "covering left")) {
			fail("the host never saw the client's all-scope line");
		}
		if (!hasLine(otherLog, teamId, c_NetChatScopeAll, "covering left")) {
			fail("the host did not relay the all-scope line to the other peer");
		}
		if (!hasLine(teamEcho, teamId, c_NetChatScopeAll, "covering left")) {
			fail("the sender never got its own local echo");
		}

		// Team-scope from the host: only same-team peers take the relay.
		if (!host.SendChat(c_NetChatScopeTeam, "push together")) {
			fail("a host team-scope send was refused");
		}
		pump(4);
		if (!hasLine(lastLines(teamPeer), 0, c_NetChatScopeTeam, "push together")) {
			fail("the teammate never received the host's team line");
		}
		for (const NetChatEntry& entry: lastLines(otherPeer)) {
			if (entry.text == "push together") {
				fail("a team line reached a peer on the other team");
			}
		}

		// Team-scope from the teammate: the host sinks it but relays to nobody outside team 0.
		if (!teamPeer.SendChat(c_NetChatScopeTeam, "team only line")) {
			fail("a client team-scope send was refused");
		}
		pump(4);
		if (!hasLine(lastLines(host), teamId, c_NetChatScopeTeam, "team only line")) {
			fail("the host never saw the teammate's team line");
		}
		for (const NetChatEntry& entry: lastLines(otherPeer)) {
			if (entry.text == "team only line") {
				fail("a relayed team line leaked to the other team");
			}
		}

		// Bounds: the cap is the shared short-text bound - inclusive at 128, refused at 129 - and
		// control bytes are stripped rather than refused.
		if (!teamPeer.SendChat(c_NetChatScopeAll, std::string(NetProtocol::c_MaxShortTextBytes, 'x'))) {
			fail("a chat line exactly at the byte cap was refused");
		}
		if (teamPeer.SendChat(c_NetChatScopeAll, std::string(NetProtocol::c_MaxShortTextBytes + 1, 'x'))) {
			fail("a chat line over the byte cap was accepted");
		}
		if (teamPeer.SendChat(c_NetChatScopeAll, std::string("bad \xC0\xAF utf8"))) {
			fail("a chat line with malformed UTF-8 was accepted");
		}
		if (teamPeer.SendChat(7, "bad scope")) {
			fail("a chat line with an unknown scope was accepted");
		}
		if (teamPeer.GetStats().chatDroppedInvalid != 3) {
			fail("invalid sends counted " + std::to_string(teamPeer.GetStats().chatDroppedInvalid) + " wanted 3");
		}
		// A control byte is stripped on send rather than refused: "a\rb" reaches the wire as "ab".
		if (!teamPeer.SendChat(c_NetChatScopeAll, "a\rb")) {
			fail("a send carrying a strippable control byte was refused");
		}
		pump(4);
		if (!hasLine(lastLines(host), teamId, c_NetChatScopeAll, "ab")) {
			fail("the stripped control byte did not arrive as \"ab\" on the host");
		}

		// Rate: five lines inside one 1s window are admitted, the sixth is dropped and counted. The
		// outbox accepts every well-formed line, so the verdict is the delivered set, not returns.
		pump(110);
		for (int i = 0; i < 6; ++i) {
			teamPeer.SendChat(c_NetChatScopeAll, "burst " + std::to_string(i));
		}
		pump(4);
		uint64_t burstDelivered = 0;
		for (const NetChatEntry& entry: lastLines(teamPeer)) {
			if (entry.text.rfind("burst ", 0) == 0) ++burstDelivered;
		}
		if (burstDelivered != 5 || teamPeer.GetStats().chatDroppedRate != 1) {
			fail("burst delivered " + std::to_string(burstDelivered) + " rate-drops " +
			     std::to_string(teamPeer.GetStats().chatDroppedRate) + " wanted 5/1");
		}

		// The local seat's window is not the host's window: after the host floods five inbound
		// lines, the client's own send must still be admitted and reach the host's sink.
		pump(110);
		for (int i = 0; i < 5; ++i) {
			host.SendChat(c_NetChatScopeAll, "host flood " + std::to_string(i));
		}
		pump(4);
		teamPeer.SendChat(c_NetChatScopeAll, "client turn");
		pump(4);
		if (!hasLine(lastLines(host), teamId, c_NetChatScopeAll, "client turn")) {
			fail("the client's own line was rate-dropped behind the host's inbound flood");
		}

		// The host's own seat filters a team line like the relay does: on team 0 it must not sink
		// the team-1 peer's line; moved onto team 1 it must.
		pump(110);
		if (!otherPeer.SendChat(c_NetChatScopeTeam, "team one huddle")) {
			fail("the opponent's team-scope send was refused");
		}
		pump(4);
		for (const NetChatEntry& entry: lastLines(host)) {
			if (entry.text == "team one huddle") {
				fail("a team-1 line reached the team-0 host's own sink");
			}
		}
		if (!hasLine(lastLines(otherPeer), otherId, c_NetChatScopeTeam, "team one huddle")) {
			fail("the opponent never got its own team line's echo");
		}
		host.SetChatTeams({{0, 1}, {teamId, 0}, {otherId, 1}});
		pump(110);
		if (!otherPeer.SendChat(c_NetChatScopeTeam, "team one huddle two")) {
			fail("the opponent's second team-scope send was refused");
		}
		pump(4);
		if (!hasLine(lastLines(host), otherId, c_NetChatScopeTeam, "team one huddle two")) {
			fail("the team-1 host did not sink the teammate's team line");
		}
		host.SetChatTeams({{0, 0}, {teamId, 0}, {otherId, 1}});

		// Before team membership is pushed the host's own seat follows the relay's rule: a
		// missing entry means "on no team", so a team line sinks on nobody - not on "team -1".
		host.SetChatTeams({});
		pump(110);
		if (!otherPeer.SendChat(c_NetChatScopeTeam, "unassigned huddle")) {
			fail("the unmapped opponent's team-scope send was refused");
		}
		pump(4);
		for (const NetChatEntry& entry: lastLines(host)) {
			if (entry.text == "unassigned huddle") {
				fail("a team line reached the host's sink with no team mapping pushed");
			}
		}
		host.SetChatTeams(teams);
		pump(110);

		// A malformed chat-typed packet is counted and tolerated, and the flood budget is real:
		// twenty of them burn the sender's window, count as malformed, and the peer stays seated.
		// Start in a fresh window - the team arm's relayed line still sits in this sender's.
		pump(110);
		std::vector<uint8_t> malformed;
		if (!NetProtocol::Encode(NetMessage{0, 0, NetChat{c_NetChatVersion, 0, c_NetChatScopeAll, 0, "hi"}}, malformed)) {
			*error = "could not encode the chat message the malformed arm mutates";
			return false;
		}
		malformed[27] = 9; // a scope byte the decoder must reject while the type still peeks as Chat
		NetPeerId otherTransportPeer = c_InvalidNetPeerId;
		for (const NetSessionPeerInfo& peer: host.GetReadyPeers()) {
			if (peer.assignedPeerId == otherId) otherTransportPeer = peer.transportPeerId;
		}
		if (otherTransportPeer == c_InvalidNetPeerId) {
			*error = "the opponent's transport peer id was never learned";
			return false;
		}
		const uint64_t malformedBefore = host.GetStats().chatDroppedMalformed;
		const uint64_t rateBefore = host.GetStats().chatDroppedRate;
		for (int i = 0; i < 20; ++i) {
			host.InjectEvent(NetTransportEvent{NetTransportEventType::PacketReceived, otherTransportPeer,
			                                 NetTransportLane::ControlReliable, malformed, {}}, hostTransport.NowMs());
		}
		if (host.GetStats().chatDroppedMalformed - malformedBefore != 20 ||
		    host.GetStats().chatDroppedRate - rateBefore != 15 || host.GetReadyPeerCount() != 2) {
			fail("malformed flood counted " +
			     std::to_string(host.GetStats().chatDroppedMalformed - malformedBefore) + "/" +
			     std::to_string(host.GetStats().chatDroppedRate - rateBefore) + " peers " +
			     std::to_string(host.GetReadyPeerCount()) + " wanted 20/15/2");
		}

		// Reconnect churn: transport ids are monotonic, so a reconnecting sender still
		// handshaking arrives under a fresh id. Every such id must share one malformed-sender
		// budget entry - one rate-map key, not one per fresh socket.
		const size_t rateKeysBefore = host.ChatRateWindowCount();
		for (NetPeerId fresh = 4000; fresh < 4000 + 40; ++fresh) {
			host.InjectEvent(NetTransportEvent{NetTransportEventType::PacketReceived, fresh,
			                                 NetTransportLane::ControlReliable, malformed, {}},
			                 hostTransport.NowMs());
		}
		const size_t rateKeysMinted = host.ChatRateWindowCount() - rateKeysBefore;
		if (rateKeysMinted > 1) {
			fail("unmapped senders minted " + std::to_string(rateKeysMinted) +
			     " rate-map entries across reconnect churn");
		}

		// The sink is bounded: past the window the oldest lines fall off. Start in a fresh rate
		// window - the burst arm above already spent this one's five.
		pump(110);
		for (int i = 0; i < 70; ++i) {
			teamPeer.SendChat(c_NetChatScopeAll, "cap line " + std::to_string(i));
			if (i % 5 == 4) pump(110);
		}
		const std::vector<NetChatEntry> capped = lastLines(teamPeer);
		if (capped.size() != 64 || capped.back().text != "cap line 69") {
			fail("the bounded sink kept " + std::to_string(capped.size()) + " after 70 sends");
		}
		if (!fails.empty()) {
			*error = fails.front();
			for (size_t i = 1; i < fails.size(); ++i) *error += "; " + fails[i];
			return false;
		}
		return true;
	}

	bool TestChatOutboxJoinRace(std::string* error) {
		// The UI enqueue path must never touch peers or state: a second thread posts lines while a
		// peer's join mutates the roster and the pump drains; every admitted line must land once.
		const uint16_t port = 43303;
		LoopbackTransport hostTransport, firstTransport, joinTransport;
		NetSession host, firstPeer, joinPeer;
		NetSessionConfig hostConfig;
		hostConfig.port = port;
		hostConfig.displayName = "Host";
		hostConfig.maxPeers = 2;
		hostConfig.heartbeatIntervalMs = 25;
		hostConfig.timeoutMs = 120000;
		NetIdentityManifest& identity = hostConfig.localIdentity;
		identity.gameVersion = "7.0.0-test";
		identity.networkProtocolVersion = NetProtocol::c_Version;
		identity.controllerFrameVersion = ControllerFrame::c_Version;
		identity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
		identity.buildId = "chat-outbox-selftest";
		identity.platform = "test";
		NetSessionConfig firstConfig = hostConfig;
		firstConfig.displayName = "First";
		++firstConfig.localNonce;
		NetSessionConfig joinConfig = hostConfig;
		joinConfig.displayName = "Late";
		joinConfig.localNonce += 2;
		if (!host.StartHost(hostTransport, hostConfig, error) ||
		    !firstPeer.StartClient(firstTransport, "loopback", firstConfig, error)) {
			return false;
		}
		const auto pumpOnce = [&]() {
			hostTransport.AdvanceTimeMs(250);
			firstTransport.AdvanceTimeMs(250);
			joinTransport.AdvanceTimeMs(250);
			host.Tick(hostTransport.NowMs());
			firstPeer.Tick(firstTransport.NowMs());
			joinPeer.Tick(joinTransport.NowMs());
		};
		for (uint64_t waited = 0; waited <= 8000 &&
		     !(host.GetReadyPeerCount() == 1 && firstPeer.IsReady()); waited += 250) {
			pumpOnce();
		}
		if (host.GetReadyPeerCount() != 1 || !firstPeer.IsReady()) {
			*error = "the outbox fixture never reached Ready on host and first peer";
			return false;
		}

		std::atomic<uint64_t> queued{0};
		std::atomic<uint64_t> echoed{0};
		std::atomic<bool> stopSending{false};
		// Closed loop: the thread posts the next line only after the previous one echoed, so at
		// most one is ever in flight - far under the 5-per-second window the drain enforces.
		std::thread sender([&]() {
			for (int i = 0; i < 100; ++i) {
				while (echoed.load() < static_cast<uint64_t>(i) && !stopSending.load()) {
					std::this_thread::yield();
				}
				if (stopSending.load()) return;
				if (host.SendChat(c_NetChatScopeAll, "line " + std::to_string(i))) ++queued;
			}
		});
		// A peer joins while the enqueue thread runs: the roster mutates under the same pumps that
		// drain the outbox, which is exactly the overlap the queue must make safe.
		std::map<std::string, uint32_t> seen;
		const auto collectEchoes = [&]() {
			for (const NetChatEntry& entry: host.TakeChatEntries()) {
				if (entry.senderPeerId == 0 && entry.text.rfind("line ", 0) == 0) ++seen[entry.text];
			}
			echoed.store(seen.size()); // every text is unique, so the map size is the line count
		};
		const auto pumpAndCount = [&]() {
			pumpOnce();
			collectEchoes(); // the sink keeps only 64, so drain it before the count, not after
		};
		for (uint64_t step = 0; step < 220; ++step) {
			if (step == 20 && !joinPeer.StartClient(joinTransport, "loopback", joinConfig, error)) {
				stopSending = true;
				sender.join();
				return false;
			}
			pumpAndCount();
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (sender.joinable() && queued.load() < 100 && std::chrono::steady_clock::now() < deadline) {
			pumpAndCount(); // keep pumping so the echoed-gated sender can finish its hundred
		}
		stopSending = true;
		sender.join();
		if (queued.load() != 100) {
			*error = "the send thread queued " + std::to_string(queued.load()) + " of 100";
			return false;
		}
		pumpAndCount(); // drain whatever the last post left in the outbox
		if (host.GetReadyPeerCount() != 2 || !joinPeer.IsReady()) {
			*error = "the mid-send join never seated: ready peers " + std::to_string(host.GetReadyPeerCount());
			return false;
		}
		uint32_t once = 0, missing = 0, duplicated = 0;
		for (int i = 0; i < 100; ++i) {
			const auto it = seen.find("line " + std::to_string(i));
			if (it == seen.end()) ++missing;
			else if (it->second == 1) ++once;
			else ++duplicated;
		}
		if (once != 100 || missing != 0 || duplicated != 0) {
			*error = "outbox echoes: once " + std::to_string(once) + " missing " +
			         std::to_string(missing) + " duplicated " + std::to_string(duplicated) + " wanted 100/0/0";
			return false;
		}
		return true;
	}

	bool TestChatSendRefusedOutsideCarry(std::string* error) {
		// LeaveWorkerMain keeps m_Session (and m_ChatSession) owned past Completed, so a bare
		// session pointer cannot be the gate: SendChat must refuse on the service state itself.
		// A line accepted in Idle/Completed/Failed has no pump left to drain it - the UI clears
		// the input box on true, so true here is text silently thrown away.
		NetMatchService service;
		service.m_Session = std::make_unique<NetSession>();
		service.m_ChatSession = service.m_Session.get();
		for (const NetMatchServiceState state :
		     {NetMatchServiceState::Idle, NetMatchServiceState::Completed, NetMatchServiceState::Failed}) {
			service.m_State = state;
			if (service.SendChat(c_NetChatScopeAll, "ghost line")) {
				*error = std::string("SendChat accepted a line while the service was ") +
				         NetMatchService::StateName(state);
				return false;
			}
		}
		service.m_State = NetMatchServiceState::Running;
		if (!service.SendChat(c_NetChatScopeAll, "live line")) {
			*error = "SendChat refused a line while the service was Running";
			return false;
		}
		return true;
	}

	bool TestAiOnlyHostSeatsNoJoiner(std::string* error) {
		// The seats a host offers come from the roster it adopted, so an AI-only round offers none.
		NetMatchServiceRequest aiOnly;
		aiOnly.host = true;
		aiOnly.dedicated = true;
		aiOnly.port = 43215;
		aiOnly.peerCount = 2;
		aiOnly.humans = 0;
		aiOnly.cpuSlots = 2;
		aiOnly.mode = NetMatchMode::CoopPvE;
		aiOnly.playerName = "Host";
		NetMatchConfig aiOnlyRoster;
		if (!NetMatchService::BuildMatchConfig(aiOnly, 0x4149304E53454154ULL, aiOnlyRoster, error)) return false;

		NetIdentityManifest manifest;
		manifest.gameVersion = "7.0.0-test";
		manifest.networkProtocolVersion = NetProtocol::c_Version;
		manifest.controllerFrameVersion = ControllerFrame::c_Version;
		manifest.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
		manifest.buildId = "ai-only-seat-selftest";
		manifest.platform = "test";

		NetMatchService service;
		NetSessionConfig hostConfig = service.BuildSessionConfig(manifest, aiOnly, aiOnlyRoster);
		hostConfig.heartbeatIntervalMs = 25;
		// NetMatchRunner::Start resolves this from the same adopted roster.
		hostConfig.readyWithoutPeers = aiOnlyRoster.peerCount == 1;
		NetSessionConfig joinerConfig = hostConfig;
		joinerConfig.displayName = "Joiner";
		joinerConfig.readyWithoutPeers = false;
		++joinerConfig.localNonce;

		LoopbackTransport hostTransport, joinerTransport;
		NetSession host, joiner;
		if (!host.StartHost(hostTransport, hostConfig, error) ||
		    !joiner.StartClient(joinerTransport, "loopback", joinerConfig, error)) {
			return false;
		}
		for (uint64_t now = 0; now <= 2000 && !joiner.IsRejected() && host.GetReadyPeerCount() == 0; now += 10) {
			host.Tick(now);
			joiner.Tick(now);
			hostTransport.AdvanceTimeMs(10);
			joinerTransport.AdvanceTimeMs(10);
		}
		if (!joiner.IsRejected() || joiner.GetRejectReason() != NetRejectReason::SessionFull ||
		    joiner.GetRejectSummary() != "session seats no remote player" || host.GetReadyPeerCount() != 0) {
			*error = "an AI-only host answered a join with maxPeers=" + std::to_string(hostConfig.maxPeers) +
			         " seated=" + std::to_string(host.GetReadyPeerCount()) + " joiner=" +
			         NetSession::StateName(joiner.GetState()) + " \"" + joiner.GetRejectSummary() + "\"";
			return false;
		}
		if (!host.IsReady()) {
			*error = "the refusal left the AI-only round at " + std::string(NetSession::StateName(host.GetState()));
			return false;
		}

		// A roster that does seat a second peer still takes its joiner.
		NetMatchServiceRequest duel = aiOnly;
		duel.dedicated = false;
		duel.port = 43216;
		duel.humans = 2;
		duel.cpuSlots = 0;
		duel.mode = NetMatchMode::PvPSkirmish;
		NetMatchConfig duelRoster;
		if (!NetMatchService::BuildMatchConfig(duel, 0x4449454C53454154ULL, duelRoster, error)) return false;
		NetSessionConfig duelHostConfig = service.BuildSessionConfig(manifest, duel, duelRoster);
		duelHostConfig.heartbeatIntervalMs = 25;
		NetSessionConfig duelJoinerConfig = duelHostConfig;
		duelJoinerConfig.displayName = "Joiner";
		++duelJoinerConfig.localNonce;
		LoopbackTransport duelHostTransport, duelJoinerTransport;
		NetSession duelHost, duelJoiner;
		if (!duelHost.StartHost(duelHostTransport, duelHostConfig, error) ||
		    !duelJoiner.StartClient(duelJoinerTransport, "loopback", duelJoinerConfig, error)) {
			return false;
		}
		for (uint64_t now = 0; now <= 2000 && duelHost.GetReadyPeerCount() == 0 && !duelJoiner.IsRejected(); now += 10) {
			duelHost.Tick(now);
			duelJoiner.Tick(now);
			duelHostTransport.AdvanceTimeMs(10);
			duelJoinerTransport.AdvanceTimeMs(10);
		}
		if (duelHost.GetReadyPeerCount() != 1 || duelJoiner.IsRejected()) {
			*error = "a two-human host offered maxPeers=" + std::to_string(duelHostConfig.maxPeers) + " and seated " +
			         std::to_string(duelHost.GetReadyPeerCount()) + "; joiner=" + NetSession::StateName(duelJoiner.GetState());
			return false;
		}
		std::cout << "[net-match-selftest] PASS ai_only_host_seats_no_joiner" << std::endl;
		return true;
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

	bool TestFinishMatchDrainsFencedDisconnect(std::string* error) {
		const uint16_t port = 43229;
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
		identity.buildId = "finish-match-fence-selftest";
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
			*error = "the FinishMatch fence fixture never seated the client on the host session";
			return false;
		}
		const NetPeerId fencedTransportPeer = service.m_Session->GetReadyPeers().front().transportPeerId;
		service.m_Session->SetReconnectHost(&service.m_ReconnectHost);
		service.m_ReconnectHost.m_Fences.push_back({fencedTransportPeer, 0, 1});

		service.m_Coordinator = std::make_unique<NetLockstepCoordinator>();
		NetLockstepCoordinator& coordinator = *service.m_Coordinator;
		coordinator.m_RelayHost = true;
		coordinator.m_State = NetLockstepState::Running;
		coordinator.m_RemotePeerIds = {2};
		coordinator.m_RemoteTransports[2] = fencedTransportPeer;
		coordinator.SetSeatStateSource(&FencedSeatState, nullptr);
		service.AttachCoordinatorSessionSink();
		const NetTransportEvent closed{NetTransportEventType::PeerDisconnected, fencedTransportPeer,
		                               NetTransportLane::ControlReliable, {}, "seat reclaimed by a newer connection"};
		coordinator.HandleEvent(closed, 0);
		if (service.m_PendingSessionEvents.size() != 1) {
			*error = "the coordinator did not hand the fenced disconnect to the service queue";
			return false;
		}

		service.FinishMatch("match over");
		if (service.GetState() != NetMatchServiceState::Completed) {
			*error = "FinishMatch did not complete the match";
			return false;
		}
		if (service.m_Session->GetStats().fencedDisconnects != 1) {
			*error = "FinishMatch dropped the fenced disconnect; fenced_disconnects=" +
			         std::to_string(service.m_Session->GetStats().fencedDisconnects);
			return false;
		}
		if (!service.m_PendingSessionEvents.empty()) {
			*error = "FinishMatch left the handover queue filled";
			return false;
		}
		std::cout << "PASS finish_match_drains_fenced_disconnect fenced_disconnects=1" << std::endl;
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

		// A refused resync keeps the running round and its session pump attached.
		const auto refusedResyncKeepsTheRound = [&](const char* what, uint16_t port, bool lostSession) -> std::string {
			const std::string prefix = std::string("refused resync, ") + what + ": ";
			LoopbackTransport hostLink, clientLink;
			NetPeerId hostSide = c_InvalidNetPeerId;
			NetPeerId clientSide = c_InvalidNetPeerId;
			NetLockstepCoordinator hostRound, clientRound;
			std::string setupError;
			if (!StartLoopbackTransports(port, hostLink, clientLink, hostSide, clientSide, &setupError) ||
			    !hostRound.Start(hostLink, cfg(1, {{2, hostSide}}, true), &setupError) || !clientRound.Start(clientLink, cfg(2, {{1, clientSide}}, false), &setupError)) {
				return prefix + setupError;
			}
			for (uint64_t now = 0; now < 80 && !clientRound.IsRunning(); now += 5) {
				hostRound.Tick(now);
				clientRound.Tick(now);
				hostLink.AdvanceTimeMs(5);
				clientLink.AdvanceTimeMs(5);
			}
			if (!clientRound.IsRunning()) {
				return prefix + "the client round never started";
			}
			NetMatchService refused;
			if (lostSession) {
				refused.m_State = NetMatchServiceState::Running;
				refused.m_Transport = std::make_unique<GnsTransport>();
				refused.m_Session = std::make_unique<NetSession>();
				refused.m_Runner = std::make_unique<NetMatchRunner>();
			}
			int pumpRuns = 0;
			struct Detach {
				~Detach() {
					ScenarioRunner::SetSessionPump(nullptr);
					ScenarioRunner::SetLockstepCoordinator(nullptr);
				}
			} detach;
			ScenarioRunner::SetLockstepCoordinator(&clientRound);
			// Ends the round on its first run, so the wait below returns at once.
			ScenarioRunner::SetSessionPump([&] {
				if (++pumpRuns == 1) clientRound.Complete("refused resync probe");
			});
			const uint64_t roundBefore = ScenarioRunner::GetLockstepRoundId();
			std::string refusal;
			const bool resynced = refused.ResyncMatch(&refusal);
			const bool attached = ScenarioRunner::HasLockstepCoordinator();
			const uint64_t roundAfter = ScenarioRunner::GetLockstepRoundId();
			NetLockstepReadyFrame probeFrame;
			std::string probeWait;
			(void)ScenarioRunner::WaitForLockstepControllerFrame(clientRound.GetStats().nextFrame, probeFrame, &probeWait);
			const bool pumpAttached = pumpRuns > 0 && probeWait.find("refused resync probe") != std::string::npos;
			if (resynced || !attached || roundAfter != roundBefore || !pumpAttached || refused.m_ResyncRetainsLocalState || refused.m_ResyncSourceRound != 0) {
				return prefix + "ResyncMatch returned " + (resynced ? "true" : "false") + " (\"" + refusal + "\"), coordinator attached " +
				       (attached ? "yes" : "no") + ", round " + std::to_string(roundBefore) + " -> " + std::to_string(roundAfter) + ", pump runs " +
				       std::to_string(pumpRuns) + " (wait: \"" + probeWait + "\"), retains local state " + (refused.m_ResyncRetainsLocalState ? "yes" : "no") +
				       ", source round " + std::to_string(refused.m_ResyncSourceRound);
			}
			return {};
		};
		std::string refusals = refusedResyncKeepsTheRound("no live match", 43244, false);
		const std::string lostSessionRefusal = refusedResyncKeepsTheRound("a lost session", 43246, true);
		if (!lostSessionRefusal.empty()) refusals += (refusals.empty() ? "" : "; ") + lostSessionRefusal;
		if (!refusals.empty()) {
			*error = refusals;
			return false;
		}
		std::cout << "PASS refused_resync_keeps_the_round cases=2" << std::endl;
		return true;
	}

	// A ready client session for the service fixture; the service's own transport never carries a round.
	bool StartServiceRematchSession(uint16_t port, LoopbackTransport& hostTransport, LoopbackTransport& clientTransport, NetSession& hostSession, NetSession& client, std::string* error) {
		NetSessionConfig hostConfig;
		hostConfig.port = port;
		hostConfig.displayName = "Host";
		hostConfig.maxPeers = 1;
		hostConfig.heartbeatIntervalMs = 25;
		hostConfig.timeoutMs = 30000;
		NetIdentityManifest& identity = hostConfig.localIdentity;
		identity.gameVersion = "7.0.0-test";
		identity.networkProtocolVersion = NetProtocol::c_Version;
		identity.controllerFrameVersion = ControllerFrame::c_Version;
		identity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
		identity.buildId = "service-rematch-selftest";
		identity.platform = "test";
		NetSessionConfig clientConfig = hostConfig;
		clientConfig.displayName = "Client";
		++clientConfig.localNonce;
		if (!hostTransport.StartHost(port, error) || !clientTransport.Connect("loopback", port, error)) return false;
		if (!hostSession.StartHost(hostTransport, hostConfig, error) || !client.StartClient(clientTransport, "loopback", clientConfig, error)) return false;
		for (uint64_t now = 0; now <= 4000 && hostSession.GetReadyPeerCount() != 1; now += 10) {
			hostSession.Tick(now);
			client.Tick(now);
			hostTransport.AdvanceTimeMs(10);
			clientTransport.AdvanceTimeMs(10);
		}
		if (hostSession.GetReadyPeerCount() != 1 || !client.IsReady()) {
			*error = "the service fixture never seated its client session";
			return false;
		}
		return true;
	}

	// One NetMatchService::ReturnToLobby on a client, from the played roster to the one it hands its runner.
	bool ServiceRematchRoster(NetMatchService& service, const NetMatchConfig& played, uint8_t localSessionPeerId, NetMatchConfig& roster, std::string* error) {
		LoopbackTransport idle;
		NetLockstepCoordinator unused;
		NetMatchRunnerConfig primed;
		primed.host = false;
		primed.matchConfig = played;
		primed.useLobbyProtocol = true;
		primed.lobbyWaitMs = 150;
		primed.lockstepWaitMs = 150;
		primed.postSessionSettleMs = 0;
		primed.postLobbySettleMs = 0;
		// Start refuses a client with no join address, after it has taken the played roster.
		if (service.m_Runner->Start(idle, *service.m_Session, unused, primed, error)) {
			*error = "the primed runner started a session it should have refused";
			return false;
		}
		if (!service.m_Session->AdoptRematchPeerId(localSessionPeerId, error)) return false;
		(void)service.m_Coordinator->TakeSeatSnapshot();
		{
			std::lock_guard<std::mutex> lock(service.m_Mutex);
			service.m_State = NetMatchServiceState::Completed;
			service.m_WorkerDone = false;
		}
		if (!service.ReturnToLobby(error)) return false;
		for (int spin = 0; spin < 20000; ++spin) {
			{
				std::lock_guard<std::mutex> lock(service.m_Mutex);
				if (service.m_WorkerDone) break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		service.JoinWorkerIfDone();
		std::lock_guard<std::mutex> lock(service.m_Mutex);
		if (!service.m_WorkerDone || !service.m_Runner) {
			*error = "the rematch worker never handed the runner back";
			return false;
		}
		roster = service.m_Runner->GetMatchConfig();
		return true;
	}

	namespace {
		// One half of a mux over a loopback link, counting what reaches it.
		class LoopbackHalfTap final: public INetTransport {
		public:
			bool StartHost(uint16_t port, std::string* error) override {
				// A mux gives both halves one port; only the listening half may take it in the loopback registry.
				return !listen || link.StartHost(port, error);
			}
			bool Connect(const std::string& address, uint16_t port, std::string* error) override {
				++connects;
				return link.Connect(address, port, error);
			}
			bool Send(NetPeerId peerId, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error, bool* congested) override {
				++sends;
				if (NetLobbyProtocol::Decode(bytes).ok) ++lobbySends;
				return link.Send(peerId, lane, bytes, error, congested);
			}
			void Disconnect(NetPeerId peerId, const std::string& reason) override {
				++disconnects;
				link.Disconnect(peerId, reason);
			}
			void Stop() override { if (beforeStop) beforeStop(); link.Stop(); }
			std::vector<NetTransportEvent> PollEvents() override {
				++polls;
				return link.PollEvents();
			}

			std::function<void()> beforeStop;
			LoopbackTransport link;
			bool listen = true;
			uint64_t polls = 0;
			uint64_t sends = 0;
			uint64_t lobbySends = 0;
			uint64_t connects = 0;
			uint64_t disconnects = 0;
		};

		// Seat the client on the mux's tagged half.
		bool StartServiceIceSession(uint16_t port, LoopbackTransport& hostTransport, LoopbackHalfTap& p2p, NetMuxTransport& mux, NetSession& hostSession, NetSession& client, std::string* error) {
			NetSessionConfig hostConfig;
			hostConfig.port = port;
			hostConfig.displayName = "Host";
			hostConfig.maxPeers = 1;
			hostConfig.heartbeatIntervalMs = 25;
			hostConfig.timeoutMs = 30000;
			NetIdentityManifest& identity = hostConfig.localIdentity;
			identity.gameVersion = "7.0.0-test";
			identity.networkProtocolVersion = NetProtocol::c_Version;
			identity.controllerFrameVersion = ControllerFrame::c_Version;
			identity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
			identity.buildId = "service-rematch-selftest";
			identity.platform = "test";
			NetSessionConfig clientConfig = hostConfig;
			clientConfig.displayName = "Client";
			++clientConfig.localNonce;
			clientConfig.p2pJoin.identity = NetIceHostIdentity("service-ice-rematch");
			clientConfig.p2pJoin.connect = [&p2p, port](INetTransport&, std::string* connectError) { return p2p.Connect("loopback", port, connectError); };
			if (!hostTransport.StartHost(port, error)) return false;
			if (!hostSession.StartHost(hostTransport, hostConfig, error) || !client.StartClientP2P(mux, clientConfig, error)) return false;
			for (uint64_t now = 0; now <= 4000 && hostSession.GetReadyPeerCount() != 1; now += 10) {
				hostSession.Tick(now);
				client.Tick(now);
				hostTransport.AdvanceTimeMs(10);
				p2p.link.AdvanceTimeMs(10);
			}
			if (hostSession.GetReadyPeerCount() != 1 || !client.IsReady()) {
				*error = "the ICE service fixture never seated its client session";
				return false;
			}
			return true;
		}
	} // namespace

	// NetMatchService::ReturnToLobby forms the next roster itself, on a client's own played round.
	// ReturnToLobby discards the round it derived from, so every case plays its own.
	bool TestServiceReturnToLobbyFormsTheNextRoster(std::string* error) {
		struct Case {
			const char* name;
			uint8_t peerCount;
			uint16_t port;
			int view; //!< 0 none, 1 this round's, 2 an earlier round's.
			std::vector<std::pair<uint8_t, uint8_t>> seats;
		};
		const std::vector<Case> cases = {
		    {"the round's own leave", 3, 43176, 0, {{1, 0}, {2, 2}}},
		    {"the round's own leave beside a live seat", 4, 43180, 0, {{1, 0}, {2, 2}, {3, 3}}},
		    {"a drop only the host's seat view shows", 4, 43184, 1, {{1, 0}, {2, 2}}},
		    {"an earlier round's seat view", 4, 43188, 2, {{1, 0}, {2, 2}, {3, 3}}},
		};
		std::string details;
		for (const Case& testCase: cases) {
			const uint8_t peerCount = testCase.peerCount;
			const uint16_t port = testCase.port;
			RematchFixture fixture;
			if (!SetUpRematchFixture(fixture, "service-return-to-lobby-" + std::to_string(port), port, peerCount, error)) return false;
			std::string step;
			auto fail = [&](const std::string& what) {
				*error = std::string("service rematch roster, ") + testCase.name + ": " + what + (step.empty() ? "" : ": " + step);
				StopRematchFixture(fixture);
				return false;
			};
			if (!LaunchRematchRound(LiveRematchPeers(fixture), &StartRematchPeer, &step)) return fail("round 1 setup failed");
			if (!PlayRematchTicks(fixture, 6)) return fail("round 1 never committed");
			RematchPeer* dropped = fixture.Client(2);
			RematchPeer* survivor = fixture.Client(3);
			if (!dropped || !survivor) return fail("round 1 did not seat lockstep peers 2 and 3");
			const NetMatchConfig played = survivor->runner.GetMatchConfig();
			const uint8_t survivorSessionPeerId = survivor->session.GetLocalPeerId();
			dropped->gone = true;
			dropped->transport.Stop();
			RematchPeer& host = fixture.Host();
			if (!PumpRematchUntil(fixture, 3000, [&] {
				    return survivor->round->GetPeerLeaveFrames().contains(2) && host.admission.GetStats().seatsDropped == 1;
			    })) {
				return fail("the drop never reached the survivor's round and the host's plane");
			}
			// The dropped seat is held for a reclaim; commits only resume once the hold runs out.
			fixture.clock.skippedMs += NetReconnectHost::c_ProvisionalExpiryMs + 1000;
			if (!PumpRematchUntil(fixture, 4000, [&] { return survivor->round->HeldSeatResolution(2) == NetLockstepHoldResolution::Expired; })) {
				return fail("the dropped seat's hold never expired on the survivor's round");
			}
			if (!PlayRematchTicks(fixture, 2) || !FinishRematchRound(fixture, &step)) return fail("round 1 did not finish");
			const uint64_t roundId = survivor->round->GetRoundId();

			LoopbackTransport serviceHostTransport, serviceClientTransport;
			NetSession serviceHostSession;
			NetMatchService service;
			service.m_IsHost = false;
			service.m_Transport = std::make_unique<GnsTransport>();
			service.m_Session = std::make_unique<NetSession>();
			service.m_Runner = std::make_unique<NetMatchRunner>();
			if (!StartServiceRematchSession(static_cast<uint16_t>(port + 2), serviceHostTransport, serviceClientTransport, serviceHostSession, *service.m_Session, &step)) {
				return fail("the service session did not come up");
			}
			service.m_Coordinator = std::move(survivor->round);
			StopRematchFixture(fixture);

			// The host's published seat view of this round, as a client's service keeps it.
			const auto viewOf = [&played](uint64_t viewRoundId, uint8_t gonePeerId) {
				NetLockstepSeatSnapshot view;
				view.senderPeerId = played.hostPeerId;
				view.sessionId = played.sessionId;
				view.roundId = viewRoundId;
				view.revision = 1;
				for (const NetMatchPlayerSlot& slot: played.players) {
					NetSeatPresenceEntry entry;
					entry.stableSeat = static_cast<uint16_t>(slot.peerId - 1);
					entry.peerId = slot.peerId;
					entry.state = slot.peerId == gonePeerId ? NetSeatPresenceState::Disconnected : NetSeatPresenceState::Present;
					view.seats.push_back(entry);
				}
				return view;
			};
			service.m_SeatPresence.Clear();
			if (testCase.view != 0) service.m_SeatPresence.ApplySnapshot(viewOf(testCase.view == 1 ? roundId : roundId + 1, 4));
			NetMatchConfig roster;
			if (!ServiceRematchRoster(service, played, survivorSessionPeerId, roster, &step)) return fail("ReturnToLobby did not form a roster");
			std::vector<std::pair<uint8_t, uint8_t>> seats;
			for (const NetMatchPlayerSlot& slot: roster.players) {
				if (!slot.cpu) seats.emplace_back(slot.peerId, slot.team);
			}
			if (static_cast<size_t>(roster.peerCount) != testCase.seats.size() || seats != testCase.seats || roster.hostPeerId != 1) {
				std::string seen;
				for (const auto& [peerId, team]: seats) seen += " " + std::to_string(peerId) + "/t" + std::to_string(team);
				step.clear();
				return fail("formed peer_count " + std::to_string(roster.peerCount) + " seats" + (seen.empty() ? " none" : seen) +
				            " on " + std::to_string(peerCount) + " played peers");
			}
			details += " " + std::to_string(peerCount) + "->" + std::to_string(roster.peerCount);
		}
		std::cout << "PASS service_return_to_lobby_roster cases=" << cases.size() << details << std::endl;

		// An ICE match plays on the mux, so its rematch lobby must run on that same mux and hand it back.
		const std::string iceRematchError = [&]() -> std::string {
			RematchFixture fixture;
			std::string step;
			if (!SetUpRematchFixture(fixture, "service-ice-rematch", 43240, 2, &step)) return "service ICE rematch: " + step;
			auto fail = [&](const std::string& what) {
				StopRematchFixture(fixture);
				return "service ICE rematch: " + what + (step.empty() ? "" : ": " + step);
			};
			if (!LaunchRematchRound(LiveRematchPeers(fixture), &StartRematchPeer, &step)) return fail("round 1 setup failed");
			if (!PlayRematchTicks(fixture, 6) || !FinishRematchRound(fixture, &step)) return fail("round 1 did not play and finish");
			RematchPeer* client = fixture.Client(2);
			if (!client) return fail("round 1 did not seat lockstep peer 2");
			const NetMatchConfig played = client->runner.GetMatchConfig();
			const uint8_t clientSessionPeerId = client->session.GetLocalPeerId();

			LoopbackTransport iceHostTransport;
			NetSession iceHostSession;
			NetMatchService service;
			auto ipHalf = std::make_unique<LoopbackHalfTap>();
			auto p2pHalf = std::make_unique<LoopbackHalfTap>();
			LoopbackHalfTap* ip = ipHalf.get();
			LoopbackHalfTap* p2p = p2pHalf.get();
			service.m_IsHost = false;
			// WorkerMain keeps an unstarted transport beside the mux of an ICE match.
			service.m_Transport = std::make_unique<GnsTransport>();
			service.m_Mux = std::make_unique<NetMuxTransport>(std::move(ipHalf), std::move(p2pHalf));
			service.m_Session = std::make_unique<NetSession>();
			service.m_Runner = std::make_unique<NetMatchRunner>();
			if (!StartServiceIceSession(43242, iceHostTransport, *p2p, *service.m_Mux, iceHostSession, *service.m_Session, &step)) {
				return fail("the service session did not come up over the ICE half");
			}
			service.m_Coordinator = std::move(client->round);
			StopRematchFixture(fixture);
			const NetPeerId iceHost = service.m_Session->GetRemoteTransportPeerId();
			if (!NetMuxTransport::IsP2P(iceHost)) return fail("fixture: the service session's host " + std::to_string(iceHost) + " is not an ICE-half peer");

			NetMuxTransport* const mux = service.m_Mux.get();
			GnsTransport* const idle = service.m_Transport.get();
			const uint64_t p2pPollsBefore = p2p->polls;
			const uint64_t p2pLobbySendsBefore = p2p->lobbySends;
			const uint64_t ipCallsBefore = ip->sends + ip->connects + ip->disconnects;
			NetMatchConfig roster;
			const bool returned = ServiceRematchRoster(service, played, clientSessionPeerId, roster, &step);
			NetMuxTransport* muxAfter = nullptr;
			GnsTransport* idleAfter = nullptr;
			std::string stateAfter;
			{
				std::lock_guard<std::mutex> lock(service.m_Mutex);
				muxAfter = service.m_Mux.get();
				idleAfter = service.m_Transport.get();
				stateAfter = NetMatchService::StateName(service.m_State);
			}
			if (!returned) return fail("ReturnToLobby on the mux failed (state " + stateAfter + ", mux " + (muxAfter == mux ? "kept" : "lost") + ")");
			step.clear();
			if (muxAfter != mux) return fail(std::string("the worker handed back ") + (muxAfter ? "a different mux" : "no mux") + ", state " + stateAfter);
			if (idleAfter != idle) return fail(std::string("the idle transport was ") + (idleAfter ? "replaced" : "taken") + ", state " + stateAfter);
			// The same mux came back, so its halves are still alive to read.
			const uint64_t p2pPolls = p2p->polls - p2pPollsBefore;
			const uint64_t p2pLobbySends = p2p->lobbySends - p2pLobbySendsBefore;
			const uint64_t ipCalls = ip->sends + ip->connects + ip->disconnects - ipCallsBefore;
			if (p2pPolls == 0 || ipCalls != 0) {
				return fail("the rematch lobby polled the ICE half " + std::to_string(p2pPolls) + " times, sent it " + std::to_string(p2pLobbySends) +
				            " lobby messages and made " + std::to_string(ipCalls) + " calls on the IP half; state " + stateAfter);
			}
			std::cout << "PASS service_ice_rematch_keeps_the_mux p2p_polls=" << p2pPolls << " p2p_lobby_sends=" << p2pLobbySends << " state=" << stateAfter << std::endl;
			return {};
		}();

		// A late peer on the host's ICE half must hear the match-over answer on that half.
		const std::string lateRejoinError = [&]() -> std::string {
			RematchFixture fixture;
			std::string step;
			if (!SetUpRematchFixture(fixture, "service-ice-late-rejoin", 43250, 2, &step)) return "service ICE late rejoin: " + step;
			auto fail = [&](const std::string& what) {
				StopRematchFixture(fixture);
				return "service ICE late rejoin: " + what + (step.empty() ? "" : ": " + step);
			};
			if (!LaunchRematchRound(LiveRematchPeers(fixture), &StartRematchPeer, &step)) return fail("round 1 setup failed");
			if (!PlayRematchTicks(fixture, 6) || !FinishRematchRound(fixture, &step)) return fail("round 1 did not play and finish");

			NetSessionConfig hostConfig;
			hostConfig.port = 43252;
			hostConfig.displayName = "Host";
			hostConfig.maxPeers = 1;
			hostConfig.heartbeatIntervalMs = 25;
			hostConfig.timeoutMs = 30000;
			NetIdentityManifest& identity = hostConfig.localIdentity;
			identity.gameVersion = "7.0.0-test";
			identity.networkProtocolVersion = NetProtocol::c_Version;
			identity.controllerFrameVersion = ControllerFrame::c_Version;
			identity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
			identity.buildId = "service-rematch-selftest";
			identity.platform = "test";
			NetSessionConfig lateConfig = hostConfig;
			lateConfig.displayName = "Late";
			++lateConfig.localNonce;

			LoopbackTransport lateLink;
			NetSession lateClient;
			NetMatchService service;
			auto ipHalf = std::make_unique<LoopbackHalfTap>();
			auto p2pHalf = std::make_unique<LoopbackHalfTap>();
			LoopbackHalfTap* ip = ipHalf.get();
			LoopbackHalfTap* p2p = p2pHalf.get();
			ip->listen = false;
			service.m_IsHost = true;
			service.m_State = NetMatchServiceState::Running;
			// WorkerMain keeps an unstarted transport beside the mux of an ICE match.
			service.m_Transport = std::make_unique<GnsTransport>();
			service.m_Mux = std::make_unique<NetMuxTransport>(std::move(ipHalf), std::move(p2pHalf));
			GnsP2PConfig hostP2P;
			hostP2P.localIdentity = NetIceHostIdentity("service-ice-late-rejoin");
			service.m_Mux->SetHostP2P(NetMatchService::c_IceVirtualPort, hostP2P);
			service.m_Session = std::make_unique<NetSession>();
			NetSession& hostSession = *service.m_Session;
			if (!hostSession.StartHost(*service.m_Mux, hostConfig, &step) || !lateClient.StartClient(lateLink, "loopback", lateConfig, &step)) {
				return fail("the late peer could not reach the host's ICE half");
			}
			for (uint64_t now = 0; now <= 4000 && hostSession.GetReadyPeerCount() != 1; now += 10) {
				hostSession.Tick(now);
				lateClient.Tick(now);
				p2p->link.AdvanceTimeMs(10);
				lateLink.AdvanceTimeMs(10);
			}
			const std::vector<NetSessionPeerInfo> readyPeers = hostSession.GetReadyPeers();
			if (readyPeers.size() != 1 || !lateClient.IsReady()) {
				return fail("fixture: the host session has " + std::to_string(readyPeers.size()) + " Ready peers and the late client is " + (lateClient.IsReady() ? "Ready" : "not Ready"));
			}
			const NetPeerId latePeer = readyPeers.front().transportPeerId;
			service.m_Coordinator = std::move(fixture.Host().round);
			StopRematchFixture(fixture);
			const bool roundUsesLatePeer = service.m_Coordinator->UsesTransportPeer(latePeer);
			if (!NetMuxTransport::IsP2P(latePeer) || roundUsesLatePeer) {
				return fail("fixture: late peer " + std::to_string(latePeer) + (NetMuxTransport::IsP2P(latePeer) ? " is tagged" : " is untagged") +
				            " and the ended round " + (roundUsesLatePeer ? "uses" : "does not use") + " it");
			}

			(void)lateLink.PollEvents();
			const uint64_t p2pSendsBefore = p2p->sends;
			const uint64_t p2pDisconnectsBefore = p2p->disconnects;
			const uint64_t ipCallsBefore = ip->sends + ip->connects + ip->disconnects;
			service.AnswerMatchOverRejoin("match over");
			lateLink.AdvanceTimeMs(10);
			int sessionEnded = 0;
			int peerDisconnects = 0;
			std::string seen;
			for (const NetTransportEvent& event: lateLink.PollEvents()) {
				if (event.type == NetTransportEventType::PeerDisconnected) {
					++peerDisconnects;
					seen += " PeerDisconnected(" + event.reason + ")";
					continue;
				}
				const NetDecodeResult decoded = NetProtocol::Decode(event.bytes);
				const NetDisconnect* disconnect = decoded.ok ? std::get_if<NetDisconnect>(&decoded.message.payload) : nullptr;
				if (!disconnect) {
					seen += " event" + std::to_string(static_cast<int>(event.type)) + (decoded.ok ? "(other payload)" : "(undecoded)");
					continue;
				}
				seen += " NetDisconnect(" + std::to_string(disconnect->disconnectReason) + "," + disconnect->message + ")";
				if (disconnect->disconnectReason == static_cast<uint16_t>(NetRejectReason::SessionEnded)) ++sessionEnded;
			}
			const uint64_t p2pSends = p2p->sends - p2pSendsBefore;
			const uint64_t p2pDisconnects = p2p->disconnects - p2pDisconnectsBefore;
			const uint64_t ipCalls = ip->sends + ip->connects + ip->disconnects - ipCallsBefore;
			if (sessionEnded != 1 || peerDisconnects != 1 || p2pDisconnects != 1 || ipCalls != 0) {
				return fail("late peer " + std::to_string(latePeer) + " received " + std::to_string(sessionEnded) + " SessionEnded disconnects and " +
				            std::to_string(peerDisconnects) + " peer disconnects in [" + seen + " ]; ICE half sends " + std::to_string(p2pSends) +
				            " disconnects " + std::to_string(p2pDisconnects) + ", IP half calls " + std::to_string(ipCalls));
			}
			std::cout << "PASS service_ice_late_rejoin_answers_on_the_mux late_peer=" << latePeer << " ice_sends=" << p2pSends << std::endl;
			return {};
		}();

		std::string iceErrors = iceRematchError;
		if (!lateRejoinError.empty()) iceErrors += (iceErrors.empty() ? "" : "; ") + lateRejoinError;
		if (!iceErrors.empty()) {
			*error = iceErrors;
			return false;
		}
		return true;
	}

	namespace {
		// Records every call the mux routes to it, so a test can name the half and the untagged id.
		class TransportTap final: public INetTransport {
		public:
			explicit TransportTap(std::string name, std::vector<std::string>* log) : m_Name(std::move(name)), m_Log(log) {}

			bool StartHost(uint16_t port, std::string* = nullptr) override {
				m_Log->push_back(m_Name + ".StartHost(" + std::to_string(port) + ")");
				return true;
			}
			bool Connect(const std::string& address, uint16_t port, std::string* = nullptr) override {
				m_Log->push_back(m_Name + ".Connect(" + address + ":" + std::to_string(port) + ")");
				return true;
			}
			bool Send(NetPeerId peerId, NetTransportLane, const std::vector<uint8_t>& bytes, std::string* = nullptr, bool* = nullptr) override {
				m_Log->push_back(m_Name + ".Send(" + std::to_string(peerId) + "," + std::to_string(bytes.size()) + ")");
				return true;
			}
			void Disconnect(NetPeerId peerId, const std::string& reason) override {
				m_Log->push_back(m_Name + ".Disconnect(" + std::to_string(peerId) + "," + reason + ")");
			}
			void Stop() override { m_Log->push_back(m_Name + ".Stop()"); }
			std::vector<NetTransportEvent> PollEvents() override {
				std::vector<NetTransportEvent> events;
				events.swap(m_Pending);
				return events;
			}
			uint32_t GetPeerPingMs(NetPeerId peerId) const override { return m_Ping + peerId; }

			void Queue(NetTransportEvent event) { m_Pending.push_back(std::move(event)); }
			void SetPing(uint32_t ping) { m_Ping = ping; }

		private:
			std::string m_Name;
			std::vector<std::string>* m_Log;
			std::vector<NetTransportEvent> m_Pending;
			uint32_t m_Ping = 0;
		};

		std::unique_ptr<NetMuxTransport> MakeTappedMux(std::vector<std::string>* log, TransportTap** ip, TransportTap** p2p) {
			auto ipTap = std::make_unique<TransportTap>("ip", log);
			auto p2pTap = std::make_unique<TransportTap>("p2p", log);
			*ip = ipTap.get();
			*p2p = p2pTap.get();
			return std::make_unique<NetMuxTransport>(std::move(ipTap), std::move(p2pTap));
		}
	} // namespace

	// The ICE listen must open before the IP one: binding the session identity is a process-wide
	// ResetIdentity that GNS refuses once any listen socket of the process is up.
	bool TestMuxOpensIceListenFirst(std::string* error) {
		std::vector<std::string> log;
		TransportTap* ip = nullptr;
		TransportTap* p2p = nullptr;
		std::unique_ptr<NetMuxTransport> mux = MakeTappedMux(&log, &ip, &p2p);

		GnsP2PConfig config;
		config.localIdentity = "str:h-deadbeef";
		mux->SetHostP2P(41011, config);
		if (!mux->HostP2PArmed() || mux->HostP2PConfig().localIdentity != "str:h-deadbeef") {
			*error = "mux listen order: the armed host identity did not stick";
			return false;
		}
		std::string startError;
		if (!mux->StartHost(41010, &startError)) {
			*error = "mux listen order: StartHost failed: " + startError;
			return false;
		}
		if (log.size() != 2 || log[0] != "p2p.StartHost(41010)" || log[1] != "ip.StartHost(41010)") {
			std::string seen;
			for (const std::string& line: log) seen += line + " ";
			*error = "mux listen order: calls were [" + seen + "], expected the ICE half first";
			return false;
		}
		std::cout << "[net-match-selftest] PASS mux listen order: the ICE half opens first (identity bound there), then the IP listen" << std::endl;
		return true;
	}

	bool TestMuxRoutesByTag(std::string* error) {
		std::vector<std::string> log;
		TransportTap* ip = nullptr;
		TransportTap* p2p = nullptr;
		std::unique_ptr<NetMuxTransport> mux = MakeTappedMux(&log, &ip, &p2p);

		const NetPeerId tagged = NetMuxTransport::Tag(3);
		if (!NetMuxTransport::IsP2P(tagged) || NetMuxTransport::Untag(tagged) != 3 || NetMuxTransport::IsP2P(3)) {
			*error = "mux routing: the peer-id tag does not round-trip";
			return false;
		}
		(void)mux->Send(7, NetTransportLane::ControlReliable, std::vector<uint8_t>(4), nullptr, nullptr);
		(void)mux->Send(tagged, NetTransportLane::InputUnreliable, std::vector<uint8_t>(9), nullptr, nullptr);
		mux->Disconnect(7, "ip-side");
		mux->Disconnect(tagged, "ice-side");
		const std::vector<std::string> wanted = {"ip.Send(7,4)", "p2p.Send(3,9)", "ip.Disconnect(7,ip-side)", "p2p.Disconnect(3,ice-side)"};
		if (log != wanted) {
			std::string seen;
			for (const std::string& line: log) seen += line + " ";
			*error = "mux routing: calls were [" + seen + "]";
			return false;
		}

		NetTransportEvent fromIp;
		fromIp.type = NetTransportEventType::PeerConnected;
		fromIp.peerId = 2;
		NetTransportEvent fromP2P;
		fromP2P.type = NetTransportEventType::PeerConnected;
		fromP2P.peerId = 5;
		NetTransportEvent localFault;
		localFault.type = NetTransportEventType::LocalTransportFault;
		localFault.peerId = c_InvalidNetPeerId;
		ip->Queue(fromIp);
		p2p->Queue(fromP2P);
		p2p->Queue(localFault);
		const std::vector<NetTransportEvent> events = mux->PollEvents();
		if (events.size() != 3 || events[0].peerId != 2 || events[1].peerId != NetMuxTransport::Tag(5) || events[2].peerId != c_InvalidNetPeerId) {
			*error = "mux routing: the polled events did not carry the ICE half's tag";
			return false;
		}

		// GnsTransport has no locks, so another thread's work runs here and nowhere else.
		int ran = 0;
		mux->Post([&ran] { ++ran; });
		if (mux->PendingTasks() != 1 || ran != 0) {
			*error = "mux routing: a posted task ran before PollEvents";
			return false;
		}
		(void)mux->PollEvents();
		if (ran != 1 || mux->PendingTasks() != 0) {
			*error = "mux routing: PollEvents did not drain the posted task";
			return false;
		}
		std::cout << "[net-match-selftest] PASS mux routing: Send/Disconnect/ping follow the peer-id tag, ICE events come back tagged, an unbound fault stays invalid, posted tasks run inside PollEvents" << std::endl;
		return true;
	}

#ifdef CCCP_WITH_GNS
	bool TestGnsStopCancelContracts(std::string* error) {
		auto fail = [&](const std::string& what) -> bool {
			if (error) *error = what;
			return false;
		};
		std::string errors;
		const auto add = [&](const std::string& what) {
			if (!what.empty()) errors += (errors.empty() ? "" : "; ") + what;
		};
		const auto waitJoinable = [](NetMatchService& service, int ms) {
			for (int i = 0; i < ms && !service.m_Worker.joinable(); ++i) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return service.m_Worker.joinable();
		};
		const auto waitDone = [](NetMatchService& service, int ms) {
			for (int i = 0; i < ms; ++i) {
				{
					std::lock_guard<std::mutex> lock(service.m_Mutex);
					if (service.m_WorkerDone) return true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return false;
		};
		const auto primeCompletedClient = [](NetMatchService& service, const NetMatchConfig& played, uint8_t peerId, std::string* step) -> bool {
			LoopbackTransport idle;
			NetLockstepCoordinator unused;
			NetMatchRunnerConfig primed;
			primed.host = false;
			primed.matchConfig = played;
			primed.useLobbyProtocol = true;
			primed.lobbyWaitMs = 8000;
			primed.lockstepWaitMs = 150;
			primed.postSessionSettleMs = 0;
			primed.postLobbySettleMs = 0;
			primed.cancelRequested = &service.m_CancelRequested;
			if (service.m_Runner->Start(idle, *service.m_Session, unused, primed, step)) {
				if (step) *step = "the primed runner started a session it should have refused";
				return false;
			}
			if (!service.m_Session->AdoptRematchPeerId(peerId, step)) return false;
			(void)service.m_Coordinator->TakeSeatSnapshot();
			{
				std::lock_guard<std::mutex> lock(service.m_Mutex);
				service.m_State = NetMatchServiceState::Completed;
				service.m_WorkerDone = false;
			}
			return true;
		};
		const auto armIceDispatcher = [](NetMatchService& service, GnsTransport& gns, std::atomic<int>& updates) -> GnsDirectorySignalDispatcher* {
			service.m_Dispatcher = std::make_unique<GnsDirectorySignalDispatcher>();
			GnsDirectorySignalDispatcher* raw = service.m_Dispatcher.get();
			GnsDirectorySignalDispatcher::Config cfg;
			(void)raw->Start(gns, cfg);
			raw->SetPolling(true, 1);
			if (service.m_Mux) {
				service.m_Mux->SetPump([raw, &updates] {
					updates.fetch_add(1);
					raw->Update(1);
				});
			}
			return raw;
		};

		// Observe the real teardown while the mux and dispatcher are still inspectable.
		for (bool runtimeError : {false, true}) {
			const std::string arm = runtimeError ? "SC5-error" : "SC5-destroy";
			GnsTransport gns;
			std::atomic<int> updates{0};
			int stops = 0;
			int halfStops = 0;
			NetMatchService service;
			auto ipHalf = std::make_unique<LoopbackHalfTap>();
			auto p2pHalf = std::make_unique<LoopbackHalfTap>();
			LoopbackHalfTap* ip = ipHalf.get();
			service.m_Mux = std::make_unique<NetMuxTransport>(std::move(ipHalf), std::move(p2pHalf));
			NetMuxTransport* mux = service.m_Mux.get();
			GnsDirectorySignalDispatcher* dispatcher = armIceDispatcher(service, gns, updates);
			auto pumpArmed = [mux] {
				std::lock_guard<std::mutex> lock(mux->m_TaskMutex);
				return static_cast<bool>(mux->m_Pump);
			};
			dispatcher->SetTrace([&](const std::string& line) {
				if (line.find("polling disarmed after") == std::string::npos) return;
				++stops;
				if (pumpArmed()) add(arm + " mux pump still armed during dispatcher Stop");
			});
			ip->beforeStop = [&] {
				++halfStops;
				if (stops != 1 || service.m_Dispatcher) add(arm + " mux stopped before dispatcher teardown");
				if (pumpArmed()) {
					add(arm + " mux pump still armed after dispatcher.reset(); PollEvents would invoke the destroyed pointee");
				} else {
					const int before = updates.load();
					(void)mux->PollEvents();
					if (updates.load() != before) add(arm + " pump ran after dispatcher.reset()");
				}
			};
			(void)mux->PollEvents();
			if (updates.load() != 1) add(arm + " live pump did not call Update exactly once");
			if (runtimeError) service.ReportRuntimeError("stop contract probe");
			else service.Destroy();
			const int after = updates.load();
			service.Destroy();
			if (stops != 1 || halfStops != 1 || updates.load() != after) add(arm + " teardown repeated or callback survived destroy");
		}

		// ICE rematch: second ReturnToLobby refuses while the worker owns the link; Destroy cancels and joins.
		{
			RematchFixture fixture;
			std::string step;
			if (!SetUpRematchFixture(fixture, "stop-cancel-ice-destroy", 43350, 2, &step)) {
				add("SC2-ice fixture: " + step);
			} else if (!LaunchRematchRound(LiveRematchPeers(fixture), &StartRematchPeer, &step) ||
			           !PlayRematchTicks(fixture, 6) || !FinishRematchRound(fixture, &step)) {
				StopRematchFixture(fixture);
				add("SC2-ice round: " + step);
			} else {
				RematchPeer* client = fixture.Client(2);
				LoopbackTransport iceHostTransport;
				NetSession iceHostSession;
				NetMatchService service;
				auto ipHalf = std::make_unique<LoopbackHalfTap>();
				auto p2pHalf = std::make_unique<LoopbackHalfTap>();
				LoopbackHalfTap* p2p = p2pHalf.get();
				service.m_IsHost = false;
				service.m_Transport = std::make_unique<GnsTransport>();
				service.m_Mux = std::make_unique<NetMuxTransport>(std::move(ipHalf), std::move(p2pHalf));
				service.m_Session = std::make_unique<NetSession>();
				service.m_Runner = std::make_unique<NetMatchRunner>();
				if (!client || !StartServiceIceSession(43352, iceHostTransport, *p2p, *service.m_Mux, iceHostSession, *service.m_Session, &step)) {
					StopRematchFixture(fixture);
					add("SC2-ice session: " + step);
				} else {
					service.m_Coordinator = std::move(client->round);
					const NetMatchConfig played = client->runner.GetMatchConfig();
					const uint8_t clientSessionPeerId = client->session.GetLocalPeerId();
					StopRematchFixture(fixture);
					GnsTransport gns;
					std::atomic<int> updates{0};
					(void)armIceDispatcher(service, gns, updates);
					if (!primeCompletedClient(service, played, clientSessionPeerId, &step)) {
						add("SC2-ice prime: " + step);
					} else {
						std::string rtl;
						const bool returned = service.ReturnToLobby(&rtl);
						if (!returned) {
							add("SC2-ice first ReturnToLobby failed (" + rtl + ")");
						} else if (!waitJoinable(service, 200)) {
							add("SC2-ice rematch worker never became joinable");
						} else {
							std::string again;
							const bool second = service.ReturnToLobby(&again);
							const bool joinable = service.m_Worker.joinable();
							if (second || !joinable || again.find("previous match worker") == std::string::npos) {
								add("SC2-ice second ReturnToLobby returned " + std::string(second ? "true" : "false") + " (\"" + again +
								    "\"), joinable " + (joinable ? "yes" : "no"));
							}
							const auto t0 = std::chrono::steady_clock::now();
							service.Destroy();
							const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
							if (ms > 2000) add("SC2-ice Destroy took " + std::to_string(ms) + " ms");
							if (service.m_Worker.joinable()) add("SC2-ice worker still joinable after Destroy");
							std::lock_guard<std::mutex> lock(service.m_Mutex);
							if (service.m_Mux || service.m_Dispatcher || service.m_Transport) {
								add("SC2-ice owners survived Destroy");
							}
						}
					}
				}
			}
		}

		// ICE rematch: cancel finishes the worker and Restore puts mux+dispatcher back on the service.
		{
			RematchFixture fixture;
			std::string step;
			if (!SetUpRematchFixture(fixture, "stop-cancel-ice-handback", 43360, 2, &step)) {
				add("SC-handback fixture: " + step);
			} else if (!LaunchRematchRound(LiveRematchPeers(fixture), &StartRematchPeer, &step) ||
			           !PlayRematchTicks(fixture, 6) || !FinishRematchRound(fixture, &step)) {
				StopRematchFixture(fixture);
				add("SC-handback round: " + step);
			} else {
				RematchPeer* client = fixture.Client(2);
				LoopbackTransport iceHostTransport;
				NetSession iceHostSession;
				NetMatchService service;
				auto ipHalf = std::make_unique<LoopbackHalfTap>();
				auto p2pHalf = std::make_unique<LoopbackHalfTap>();
				LoopbackHalfTap* p2p = p2pHalf.get();
				service.m_IsHost = false;
				service.m_Transport = std::make_unique<GnsTransport>();
				service.m_Mux = std::make_unique<NetMuxTransport>(std::move(ipHalf), std::move(p2pHalf));
				service.m_Session = std::make_unique<NetSession>();
				service.m_Runner = std::make_unique<NetMatchRunner>();
				if (!client || !StartServiceIceSession(43362, iceHostTransport, *p2p, *service.m_Mux, iceHostSession, *service.m_Session, &step)) {
					StopRematchFixture(fixture);
					add("SC-handback session: " + step);
				} else {
					service.m_Coordinator = std::move(client->round);
					const NetMatchConfig played = client->runner.GetMatchConfig();
					const uint8_t clientSessionPeerId = client->session.GetLocalPeerId();
					StopRematchFixture(fixture);
					GnsTransport gns;
					std::atomic<int> updates{0};
					NetMuxTransport* const muxBefore = service.m_Mux.get();
					GnsDirectorySignalDispatcher* const dispBefore = armIceDispatcher(service, gns, updates);
					if (!primeCompletedClient(service, played, clientSessionPeerId, &step)) {
						add("SC-handback prime: " + step);
					} else {
						std::string rtl;
						if (!service.ReturnToLobby(&rtl)) {
							add("SC-handback first ReturnToLobby failed (" + rtl + ")");
						} else if (!waitJoinable(service, 200)) {
							add("SC-handback rematch worker never became joinable");
						} else {
							{
								std::lock_guard<std::mutex> lock(service.m_Mutex);
								if (service.m_Mux || service.m_Dispatcher) {
									add("SC-handback owners were not taken by the rematch worker");
								}
							}
							service.m_CancelRequested.store(true);
							if (!waitDone(service, 2000)) {
								add("SC-handback cancel did not finish the worker");
							}
							service.JoinWorkerIfDone();
							{
								std::lock_guard<std::mutex> lock(service.m_Mutex);
								if (service.m_Mux.get() != muxBefore) {
									add("SC-handback mux was not restored");
								}
								if (service.m_Dispatcher.get() != dispBefore) {
									add("SC-handback dispatcher was not restored");
								}
							}
							service.Destroy();
							if (service.m_Worker.joinable()) add("SC-handback worker still joinable after Destroy");
						}
					}
				}
			}
		}

		// IP rematch: same second-ReturnToLobby and Destroy contracts on the IP wire.
		{
			RematchFixture fixture;
			std::string step;
			if (!SetUpRematchFixture(fixture, "stop-cancel-ip", 43370, 2, &step)) {
				add("SC2-ip fixture: " + step);
			} else if (!LaunchRematchRound(LiveRematchPeers(fixture), &StartRematchPeer, &step) ||
			           !PlayRematchTicks(fixture, 6) || !FinishRematchRound(fixture, &step)) {
				StopRematchFixture(fixture);
				add("SC2-ip round: " + step);
			} else {
				RematchPeer* client = fixture.Client(2);
				LoopbackTransport hostTransport;
				LoopbackTransport clientTransport;
				NetSession hostSession;
				NetMatchService service;
				service.m_IsHost = false;
				service.m_Transport = std::make_unique<GnsTransport>();
				service.m_Session = std::make_unique<NetSession>();
				service.m_Runner = std::make_unique<NetMatchRunner>();
				if (!client || !StartServiceRematchSession(43372, hostTransport, clientTransport, hostSession, *service.m_Session, &step)) {
					StopRematchFixture(fixture);
					add("SC2-ip session: " + step);
				} else {
					service.m_Coordinator = std::move(client->round);
					const NetMatchConfig played = client->runner.GetMatchConfig();
					const uint8_t clientSessionPeerId = client->session.GetLocalPeerId();
					StopRematchFixture(fixture);
					if (!primeCompletedClient(service, played, clientSessionPeerId, &step)) {
						add("SC2-ip prime: " + step);
					} else {
						std::string rtl;
						if (!service.ReturnToLobby(&rtl)) {
							add("SC2-ip first ReturnToLobby failed: " + rtl);
						} else if (!waitJoinable(service, 200)) {
							add("SC2-ip rematch worker never became joinable");
						} else {
							std::string again;
							const bool second = service.ReturnToLobby(&again);
							const bool joinable = service.m_Worker.joinable();
							if (second) {
								add("SC2-ip second ReturnToLobby started another worker (\"" + again + "\")");
							} else if (!joinable) {
								add("SC2-ip second ReturnToLobby left no joinable worker (\"" + again + "\")");
							} else if (again.find("previous match worker") == std::string::npos) {
								add("SC2-ip second ReturnToLobby \"" + again + "\"");
							}
							const auto t0 = std::chrono::steady_clock::now();
							service.Destroy();
							const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
							if (ms > 2000) add("SC2-ip Destroy took " + std::to_string(ms) + " ms");
							if (service.m_Worker.joinable()) add("SC2-ip worker still joinable after Destroy");
						}
					}
				}
			}
		}

		if (!errors.empty()) return fail(errors);
		std::cout << "PASS gns_stop_cancel_contracts" << std::endl;
		return true;
	}
#else
	bool TestGnsStopCancelContracts(std::string* error) {
		(void)error;
		return true;
	}
#endif

	bool TestEndedWorldLateAdmission(std::string* error) {
		auto fail = [&](const std::string& what) -> bool {
			if (error) *error = what;
			return false;
		};
		std::string errors;
		const auto add = [&](const std::string& what) {
			if (!what.empty()) errors += (errors.empty() ? "" : "; ") + what;
		};

		{
			NetMessage message;
			message.payload = NetDisconnect{static_cast<uint16_t>(NetRejectReason::SessionEnded), "match over"};
			std::vector<uint8_t> bytes;
			if (!NetProtocol::Encode(message, bytes)) return fail("SessionEnded packet did not encode");
			const NetDecodeResult decoded = NetProtocol::Decode(bytes);
			const NetDisconnect* disconnect = decoded.ok ? std::get_if<NetDisconnect>(&decoded.message.payload) : nullptr;
			if (!disconnect || disconnect->disconnectReason != static_cast<uint16_t>(NetRejectReason::SessionEnded)) {
				return fail("SessionEnded packet did not decode");
			}
		}

		const auto countEndedPackets = [](const std::vector<NetTransportEvent>& events, int& sessionEnded, int& peerDisconnects, std::string& seen) {
			sessionEnded = 0;
			peerDisconnects = 0;
			seen.clear();
			for (const NetTransportEvent& event: events) {
				if (event.type == NetTransportEventType::PeerDisconnected) {
					++peerDisconnects;
					seen += " PeerDisconnected(" + event.reason + ")";
					continue;
				}
				const NetDecodeResult decoded = NetProtocol::Decode(event.bytes);
				const NetDisconnect* disconnect = decoded.ok ? std::get_if<NetDisconnect>(&decoded.message.payload) : nullptr;
				if (!disconnect) {
					seen += " event" + std::to_string(static_cast<int>(event.type)) + (decoded.ok ? "(other payload)" : "(undecoded)");
					continue;
				}
				seen += " NetDisconnect(" + std::to_string(disconnect->disconnectReason) + "," + disconnect->message + ")";
				if (disconnect->disconnectReason == static_cast<uint16_t>(NetRejectReason::SessionEnded)) ++sessionEnded;
			}
		};

		const auto endedRefuseVerdict = [](const char* path, int sessionEnded, int peerDisconnects, uint32_t reseats, bool usesLate, bool dispatcherOwned) -> std::string {
			if (reseats != 0 || usesLate) {
				return std::string(path) + " reseated/admitted late peer into the old epoch reseats=" +
				       std::to_string(reseats) + " uses_late=" + (usesLate ? "1" : "0");
			}
			if (sessionEnded != 1 || peerDisconnects != 1) {
				return std::string(path) + " received " + std::to_string(sessionEnded) +
				       " SessionEnded disconnects and " + std::to_string(peerDisconnects) + " peer disconnects";
			}
			if (!dispatcherOwned) {
				return std::string(path) + " dispatcher pump was not owned after the ended-world refuse";
			}
			return {};
		};

		const auto runLatePath = [&](bool iceHalf, uint16_t sessionPort, uint16_t coordPort) -> std::string {
			const char* path = iceHalf ? "mux" : "ip";
			LoopbackTransport coordHost, coordClient;
			NetPeerId coordRemote = c_InvalidNetPeerId;
			NetPeerId coordLocal = c_InvalidNetPeerId;
			std::string step;
			if (!StartLoopbackTransports(coordPort, coordHost, coordClient, coordRemote, coordLocal, &step)) {
				return std::string(path) + " coordinator loopback: " + step;
			}

			NetMatchService service;
			auto ipHalfPtr = std::make_unique<LoopbackHalfTap>();
			auto p2pHalfPtr = std::make_unique<LoopbackHalfTap>();
			LoopbackHalfTap* ip = ipHalfPtr.get();
			LoopbackHalfTap* p2p = p2pHalfPtr.get();
			if (iceHalf) {
				ip->listen = false;
			} else {
				p2p->listen = false;
			}
			service.m_IsHost = true;
			service.m_State = NetMatchServiceState::Running;
			service.m_ResyncOnDesync = true;
			service.m_Transport = std::make_unique<GnsTransport>();
			service.m_Mux = std::make_unique<NetMuxTransport>(std::move(ipHalfPtr), std::move(p2pHalfPtr));
			if (iceHalf) {
				GnsP2PConfig hostP2P;
				hostP2P.localIdentity = NetIceHostIdentity("ended-world-late-mux");
				service.m_Mux->SetHostP2P(NetMatchService::c_IceVirtualPort, hostP2P);
			}
			service.m_Session = std::make_unique<NetSession>();
			service.m_Runner = std::make_unique<NetMatchRunner>();
			service.m_Coordinator = std::make_unique<NetLockstepCoordinator>();

			NetLockstepConfig coordCfg;
			coordCfg.sessionId = 0x454E4452443031ULL;
			coordCfg.timeoutMs = 2000;
			coordCfg.localPeerId = 1;
			coordCfg.peerCount = 2;
			coordCfg.remoteTransportPeerIds = {{2, coordRemote}};
			coordCfg.relayToOtherPeers = true;
			coordCfg.scenario = "LockstepSelfTest";
			coordCfg.ownershipPolicy = "unique-id-split";
			coordCfg.roundId = 77;
			if (!service.m_Coordinator->Start(coordHost, coordCfg, &step)) {
				return std::string(path) + " coordinator Start: " + step;
			}
			const uint64_t oldEpoch = service.m_Coordinator->GetRoundId();
			if (oldEpoch != 77) {
				return std::string(path) + " fixture: old epoch was " + std::to_string(oldEpoch);
			}

			NetSessionConfig hostConfig;
			hostConfig.port = sessionPort;
			hostConfig.displayName = "Host";
			hostConfig.maxPeers = 1;
			hostConfig.heartbeatIntervalMs = 25;
			hostConfig.timeoutMs = 30000;
			NetIdentityManifest& identity = hostConfig.localIdentity;
			identity.gameVersion = "7.0.0-test";
			identity.networkProtocolVersion = NetProtocol::c_Version;
			identity.controllerFrameVersion = ControllerFrame::c_Version;
			identity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
			identity.buildId = iceHalf ? "ended-world-late-mux" : "ended-world-late-ip";
			identity.platform = "test";
			NetSessionConfig lateConfig = hostConfig;
			lateConfig.displayName = "Late";
			++lateConfig.localNonce;

			LoopbackTransport lateLink;
			NetSession lateClient;
			if (!service.m_Session->StartHost(*service.m_Mux, hostConfig, &step)) return std::string(path) + " host Start: " + step;
			// Consume an actual connection id so the late IP peer is absent from the ended round.
			LoopbackTransport priorLink;
			if (!priorLink.Connect("loopback", sessionPort, &step)) return std::string(path) + " prior link: " + step;
			priorLink.Stop();
			if (!lateClient.StartClient(lateLink, "loopback", lateConfig, &step)) {
				return std::string(path) + " late peer could not reach the host: " + step;
			}
			LoopbackTransport& hostHalf = iceHalf ? p2p->link : ip->link;
			for (uint64_t now = 0; now <= 4000 && service.m_Session->GetReadyPeerCount() != 1; now += 10) {
				service.m_Session->Tick(now);
				lateClient.Tick(now);
				hostHalf.AdvanceTimeMs(10);
				lateLink.AdvanceTimeMs(10);
			}
			const std::vector<NetSessionPeerInfo> readyPeers = service.m_Session->GetReadyPeers();
			if (readyPeers.size() != 1 || !lateClient.IsReady()) {
				return std::string(path) + " fixture: host Ready peers=" + std::to_string(readyPeers.size()) +
				       " late " + (lateClient.IsReady() ? "Ready" : "not Ready");
			}
			const NetPeerId latePeer = readyPeers.front().transportPeerId;
			if (iceHalf ? !NetMuxTransport::IsP2P(latePeer) : NetMuxTransport::IsP2P(latePeer)) {
				return std::string(path) + " fixture: late peer " + std::to_string(latePeer) +
				       (NetMuxTransport::IsP2P(latePeer) ? " is tagged" : " is untagged");
			}
			if (service.m_Coordinator->UsesTransportPeer(latePeer)) {
				return std::string(path) + " fixture: the old epoch already uses late peer " + std::to_string(latePeer);
			}

			service.m_ReconnectHost.SetLiveMatch(true);
			service.m_AdmissionAttached = true;
#ifdef CCCP_WITH_GNS
			GnsTransport dispatcherWire;
			service.m_Dispatcher = std::make_unique<GnsDirectorySignalDispatcher>();
			GnsDirectorySignalDispatcher* const dispatcher = service.m_Dispatcher.get();
			GnsDirectorySignalDispatcher::Config cfg;
			(void)dispatcher->Start(dispatcherWire, cfg);
			std::atomic<int> pumps{0};
			service.m_Mux->SetPump([dispatcher, &pumps] {
				pumps.fetch_add(1);
				dispatcher->Update(1);
			});
			const int pumpsBeforeFinish = pumps.load();
			(void)service.m_Mux->PollEvents();
			if (pumps.load() <= pumpsBeforeFinish) {
				return std::string(path) + " fixture: mux pump was not owned before FinishMatch";
			}
#endif
			(void)lateLink.PollEvents();
			const uint64_t p2pSendsBefore = p2p->sends;
			const uint64_t p2pDisconnectsBefore = p2p->disconnects;
			const uint64_t ipCallsBefore = ip->sends + ip->connects + ip->disconnects;
			service.FinishMatch("match over");
			if (service.GetState() != NetMatchServiceState::Completed) {
				return std::string(path) + " FinishMatch did not complete the match";
			}
			if (!service.m_DirectoryRetracted) {
				return std::string(path) + " FinishMatch left directoryWanted live (listing not retracted)";
			}
#ifdef CCCP_WITH_GNS
			if (service.m_Dispatcher.get() != dispatcher) {
				return std::string(path) + " FinishMatch took the dispatcher";
			}
#endif
			if (service.m_Coordinator->GetRoundId() != oldEpoch) {
				return std::string(path) + " FinishMatch replaced the old epoch " + std::to_string(oldEpoch) +
				       " with " + std::to_string(service.m_Coordinator->GetRoundId());
			}

			service.PumpSessionEvents();
#ifdef CCCP_WITH_GNS
			if (service.m_Dispatcher.get() != dispatcher) {
				return std::string(path) + " ended-world pump took the dispatcher";
			}
			const int pumpsBeforeEndedPoll = pumps.load();
			(void)service.m_Mux->PollEvents();
			if (pumps.load() <= pumpsBeforeEndedPoll) {
				return std::string(path) + " dispatcher pump was not owned after the ended-world refuse";
			}
#endif
			const bool dispatcherOwned = true;
			if (service.GetState() != NetMatchServiceState::Completed) {
				return std::string(path) + " ended-world pump left state " +
				       std::string(NetMatchService::StateName(service.GetState()));
			}
			const uint32_t reseats = service.m_ReconnectHost.GetStats().reseatsIssued +
			                         service.m_ReconnectHost.GetStats().reseatsWithoutALedger;
			const bool queuedReseat = !service.m_ReconnectHost.TakePendingReseats().empty();
			const bool usesLate = service.m_Coordinator && service.m_Coordinator->UsesTransportPeer(latePeer);
			lateLink.AdvanceTimeMs(10);
			int sessionEnded = 0;
			int peerDisconnects = 0;
			std::string seen;
			countEndedPackets(lateLink.PollEvents(), sessionEnded, peerDisconnects, seen);
			const uint64_t p2pSends = p2p->sends - p2pSendsBefore;
			const uint64_t p2pDisconnects = p2p->disconnects - p2pDisconnectsBefore;
			const uint64_t ipCalls = ip->sends + ip->connects + ip->disconnects - ipCallsBefore;
			std::string verdict = endedRefuseVerdict(path, sessionEnded, peerDisconnects, reseats + (queuedReseat ? 1u : 0u),
			                                         usesLate, dispatcherOwned);
			if (!verdict.empty()) {
				return "ended-world late peer " + std::to_string(latePeer) + " on " + verdict + " in [" + seen +
				       "]; ICE half sends " + std::to_string(p2pSends) + " disconnects " +
				       std::to_string(p2pDisconnects) + ", IP half calls " + std::to_string(ipCalls);
			}
			LoopbackTransport freshLink;
			NetSession freshClient;
			lateConfig.localNonce += 1;
			if (!freshClient.StartClient(freshLink, "loopback", lateConfig, &step)) return std::string(path) + " fresh late connect: " + step;
			int freshEnded = 0, freshClosed = 0;
			bool wasReady = false;
			for (uint64_t now = 0; now <= 4000 && (freshEnded == 0 || freshClosed == 0); now += 10) {
				service.PumpSessionEvents();
				const auto events = freshLink.PollEvents();
				int ended = 0, closed = 0;
				std::string ignored;
				countEndedPackets(events, ended, closed, ignored);
				freshEnded += ended;
				freshClosed += closed;
				for (const auto& event : events) freshClient.InjectEvent(event, now);
				freshClient.Tick(now, false);
				wasReady = wasReady || freshClient.IsReady();
				hostHalf.AdvanceTimeMs(10);
				freshLink.AdvanceTimeMs(10);
			}
			if (!wasReady || freshEnded != 1 || freshClosed != 1 || service.m_Session->GetReadyPeerCount() != 0 ||
			    service.m_ReconnectHost.GetStats().reseatsIssued != 0 || service.m_ReconnectHost.GetStats().reseatsWithoutALedger != 0) {
				return std::string(path) + " fresh arrival after FinishMatch ready=" + std::to_string(wasReady) +
				       " SessionEnded=" + std::to_string(freshEnded) + " closed=" + std::to_string(freshClosed);
			}
			std::cout << "PASS fresh_peer_after_finish path=" << path << " SessionEnded=1 reseats=0" << std::endl;
			return {};
		};

		add(runLatePath(false, 43360, 43361));
		add(runLatePath(true, 43370, 43371));

		{
			LoopbackTransport hostTransport, clientTransport;
			std::string step;
			NetMatchService service;
			service.m_IsHost = true;
			service.m_State = NetMatchServiceState::Running;
			service.m_Transport = std::make_unique<GnsTransport>();
			service.m_Session = std::make_unique<NetSession>();
			service.m_Runner = std::make_unique<NetMatchRunner>();
			service.m_Coordinator = std::make_unique<NetLockstepCoordinator>();
			NetSession client;
			NetSessionConfig hostConfig;
			hostConfig.port = 43380;
			hostConfig.displayName = "Host";
			hostConfig.maxPeers = 1;
			hostConfig.heartbeatIntervalMs = 25;
			NetIdentityManifest& identity = hostConfig.localIdentity;
			identity.gameVersion = "7.0.0-test";
			identity.networkProtocolVersion = NetProtocol::c_Version;
			identity.controllerFrameVersion = ControllerFrame::c_Version;
			identity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
			identity.buildId = "ended-world-rtl-new-round";
			identity.platform = "test";
			NetSessionConfig clientConfig = hostConfig;
			clientConfig.displayName = "Client";
			++clientConfig.localNonce;
			if (!service.m_Session->StartHost(hostTransport, hostConfig, &step) ||
			    !client.StartClient(clientTransport, "loopback", clientConfig, &step)) {
				add("ReturnToLobby fixture session: " + step);
			} else {
				for (uint64_t now = 0; now <= 2000 && service.m_Session->GetReadyPeerCount() != 1; now += 10) {
					service.m_Session->Tick(now);
					client.Tick(now);
					hostTransport.AdvanceTimeMs(10);
					clientTransport.AdvanceTimeMs(10);
				}
				if (service.m_Session->GetReadyPeerCount() != 1) {
					add("ReturnToLobby fixture never seated the original peer");
				} else {
					service.FinishMatch("match over");
					if (service.GetState() != NetMatchServiceState::Completed) {
						add("ReturnToLobby control: FinishMatch did not complete");
					} else {
						std::string rtl;
						const bool returned = service.ReturnToLobby(&rtl);
						NetMatchServiceState stateAfter;
						{
							std::lock_guard<std::mutex> lock(service.m_Mutex);
							stateAfter = service.m_State;
						}
						if (!returned) {
							add("ReturnToLobby after FinishMatch was refused (" + rtl + ", state " +
							    NetMatchService::StateName(stateAfter) + ")");
						} else if (stateAfter == NetMatchServiceState::Completed) {
							add("ReturnToLobby after FinishMatch left the ended world");
						}
						service.Destroy();
					}
				}
			}
		}

		if (!errors.empty()) {
			*error = errors;
			return false;
		}
		std::cout << "PASS ended_world_late_admission ip+mux SessionEnded=1 reseats=0 rtl_new_round=1" << std::endl;
		return true;
	}

	bool TestServiceIceRematchPlaysTwoRounds(std::string* error) {
		class Half final : public INetTransport {
		public:
			bool listen = true;
			LockedLoopback link;
			std::atomic<uint32_t> binds{0}, sends{0}, lobbySends{0}, connects{0};
			bool StartHost(uint16_t port, std::string* e) override { ++binds; return !listen || link.StartHost(port, e); }
			bool Connect(const std::string& address, uint16_t port, std::string* e) override { ++connects; return link.Connect(address, port, e); }
			bool Send(NetPeerId peer, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* e, bool* congested) override {
				++sends;
				if (NetLobbyProtocol::Decode(bytes).ok) ++lobbySends;
				return link.Send(peer, lane, bytes, e, congested);
			}
			void Disconnect(NetPeerId peer, const std::string& reason) override { link.Disconnect(peer, reason); }
			void Stop() override { link.Stop(); }
			std::vector<NetTransportEvent> PollEvents() override { return link.PollEvents(); }
		};
		std::array<NetMatchService, 2> peers;
		std::array<Half*, 2> ip{}, ice{};
		std::array<NetMuxTransport*, 2> bound{};
		std::array<NetMatchRunnerConfig, 2> configs;
		const std::string rowId = "10a0c8de-0000-4000-8000-000000000006";
		const std::string identity = NetIceHostIdentity(rowId);
		const auto fail = [&](const std::string& message) {
			for (size_t index = 0; index < peers.size(); ++index) {
				std::cout << "[net-match-selftest] MEASURE ice round diagnostic peer=" << index << ' ' << peers[index].BuildReportJson() << std::endl;
			}
			for (auto& peer : peers) peer.Destroy();
			*error = "service ICE two rounds: " + message;
			return false;
		};
		for (size_t index = 0; index < peers.size(); ++index) {
			NetMatchService& peer = peers[index];
			auto ipHalf = std::make_unique<Half>();
			auto iceHalf = std::make_unique<Half>();
			ip[index] = ipHalf.get();
			ice[index] = iceHalf.get();
			ip[index]->listen = false;
			peer.m_IsHost = index == 0;
			peer.m_Transport = std::make_unique<GnsTransport>();
			peer.m_Mux = std::make_unique<NetMuxTransport>(std::move(ipHalf), std::move(iceHalf));
			bound[index] = peer.m_Mux.get();
			peer.m_Session = std::make_unique<NetSession>();
			peer.m_Runner = std::make_unique<NetMatchRunner>();
			peer.m_Coordinator = std::make_unique<NetLockstepCoordinator>();
			peer.m_State = NetMatchServiceState::Starting;
			peer.m_AdmissionClock.Start(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
			NetMatchRunnerConfig& config = configs[index];
			config.host = peer.m_IsHost;
			config.joinAddress = peer.m_IsHost ? "" : identity;
			config.matchConfig = MakeConfig();
			config.useLobbyProtocol = true;
			config.sessionWaitMs = config.lobbyWaitMs = config.lockstepWaitMs = 4000;
			config.missingFrameGraceMs = 10000;
			config.postSessionSettleMs = config.postLobbySettleMs = 0;
			config.startFrame = 1;
			config.scenario = "ice-service-two-rounds";
			config.cancelRequested = &peer.m_CancelRequested;
			config.nowMs = [&peer] { return peer.AdmissionNowMs(); };
			NetSessionConfig& session = config.sessionConfig;
			session.port = 48041;
			session.maxPeers = 1;
			session.timeoutMs = 500;
			session.heartbeatIntervalMs = 25;
			session.sessionId = config.matchConfig.sessionId;
			session.localNonce += index;
			session.displayName = peer.m_IsHost ? "Host" : "Client";
			session.localIdentity.gameVersion = "7.0.0-test";
			session.localIdentity.networkProtocolVersion = NetProtocol::c_Version;
			session.localIdentity.controllerFrameVersion = ControllerFrame::c_Version;
			session.localIdentity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
			session.localIdentity.buildId = "ice-service-two-rounds";
			session.localIdentity.platform = "test";
			if (peer.m_IsHost) {
				GnsP2PConfig p2p;
				p2p.localIdentity = identity;
				peer.m_Mux->SetHostP2P(NetMatchService::c_IceVirtualPort, p2p);
			} else {
				session.p2pJoin.identity = identity;
				session.p2pJoin.connect = [half = ice[index]](INetTransport&, std::string* e) { return half->Connect("loopback", 48041, e); };
			}
			peer.m_Worker = std::thread([&peer, &config] {
				std::string setupError;
				const bool started = peer.m_Runner->Start(*peer.m_Mux, *peer.m_Session, *peer.m_Coordinator, config, &setupError);
				std::lock_guard<std::mutex> lock(peer.m_Mutex);
				peer.m_State = started ? NetMatchServiceState::ReadyToLaunch : NetMatchServiceState::Failed;
				peer.m_ErrorText = setupError;
				peer.m_WorkerDone = true;
			});
			if (index == 0) {
				for (int spin = 0; spin < 2000 && !ice[0]->link.IsHosting(); ++spin) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		const auto settle = [&]() -> bool {
			for (int spin = 0; spin < 6000; ++spin) {
				bool settled = true;
				for (auto& peer : peers) {
					peer.JoinWorkerIfDone();
					if (peer.m_Worker.joinable()) { settled = false; continue; }
					if (peer.GetState() == NetMatchServiceState::Failed) return false;
					if (peer.m_Coordinator) peer.m_Coordinator->Tick(NetLockstepNowMs());
				}
				if (settled) return true;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return false;
		};
		const auto play = [&](int round) -> bool {
			std::array<uint64_t, 2> produced{1, 1}, applied{0, 0};
			std::array<std::map<uint64_t, std::string>, 2> traces;
			for (size_t index = 0; index < peers.size(); ++index) {
				auto& peer = peers[index];
				peer.m_State = NetMatchServiceState::Running;
				peer.m_LocalPeerId = static_cast<uint8_t>(index + 1);
				peer.AttachCoordinatorSessionSink();
				if (peer.m_Mux.get() != bound[index] || !peer.m_Coordinator->IsRunning() ||
				    !NetMuxTransport::IsP2P(peer.m_Session->GetReadyPeers().front().transportPeerId)) return false;
			}
			for (int spin = 0; spin < 4000 && (applied[0] < 24 || applied[1] < 24); ++spin) {
				for (size_t index = 0; index < peers.size(); ++index) {
					auto& peer = peers[index];
					auto& coordinator = *peer.m_Coordinator;
					std::string sendError;
					while (produced[index] <= 24 && produced[index] <= applied[index] + 2 &&
					       coordinator.QueueLocalInput(produced[index], {RematchFrame(static_cast<uint8_t>(index + 1), produced[index])}, {}, &sendError)) ++produced[index];
					coordinator.Tick(NetLockstepNowMs());
					peer.PumpSessionEvents();
					NetLockstepReadyFrame ready;
					while (coordinator.PopReadyFrame(ready)) {
						traces[index][ready.frame] = DescribeRematchFrame(ready, static_cast<uint8_t>(index + 1));
						applied[index] = ready.frame;
						(void)coordinator.FinishSimulationTick(ready.frame);
					}
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			if (applied[0] != 24 || applied[1] != 24 || traces[0] != traces[1]) return false;
			std::cout << "PASS service_ice_round round=" << round << " ticks=24 exact_frames=24" << std::endl;
			return true;
		};
		if (!settle()) return fail("round 1 setup: " + peers[0].GetErrorText() + "; " + peers[1].GetErrorText());
		if (!play(1)) return fail("round 1 did not apply the same 24 frames");
		for (auto& peer : peers) peer.FinishMatch("round over");
		std::string rtl;
		const uint32_t lobbyBefore = ice[0]->lobbySends.load() + ice[1]->lobbySends.load();
		if (!peers[0].ReturnToLobby(&rtl)) return fail("host ReturnToLobby: " + rtl);
		const auto rematchAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
		while (std::chrono::steady_clock::now() < rematchAt) {
			peers[1].PumpSessionEvents();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (peers[1].m_PendingLobbyEvents.empty()) return fail("the completed client retained no lobby packets");
		if (!peers[1].ReturnToLobby(&rtl)) return fail("client ReturnToLobby: " + rtl);
		if (!settle()) return fail("round 2 setup: " + peers[0].GetErrorText() + "; " + peers[1].GetErrorText());
		if (!play(2)) return fail("round 2 did not apply the same 24 frames");
		if (ice[0]->binds != 1 || ice[1]->connects != 1 || ip[0]->sends != 0 || ip[1]->sends != 0 ||
		    ice[0]->lobbySends + ice[1]->lobbySends <= lobbyBefore || bound[0]->HostP2PConfig().localIdentity != identity) {
			return fail("rematch replaced the bind, redialled or used the IP half");
		}
		peers[1].LeaveMatch("player left");
		if (peers[1].ReturnToLobby(&rtl) || rtl != "this match was left") return fail("a deliberate leave offered another rematch: " + rtl);
		for (auto& peer : peers) peer.Destroy();
		std::cout << "PASS service_ice_rematch_two_rounds same_bind=1 connects=1 ip_sends=0 delayed_client_ms=1200" << std::endl;
		return true;
	}

	// The menus route a recovery pump to the title screen and keep it alive past Back; an ordinary
	// match end is not one, so it must answer the lobby pump's read instead.
	bool TestCompletedLobbyIsNotARecovery(std::string* error) {
		NetMatchService service;
		{
			std::lock_guard<std::mutex> lock(service.m_Mutex);
			service.m_IsHost = true;
			service.m_MatchWasRunning = true;
			service.m_State = NetMatchServiceState::Completed;
		}
		if (service.NeedsRecoveryPump()) {
			*error = "a completed match reads as a recovery the menus route to the title screen";
			return false;
		}
		if (!service.NeedsCompletedLobbyPump()) {
			*error = "a completed match does not ask the menu loop to pump its rematch lobby";
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(service.m_Mutex);
			service.m_LeftMatch = true;
		}
		if (service.NeedsCompletedLobbyPump()) {
			*error = "a deliberately left match still asks the menu loop to pump it";
			return false;
		}
		service.m_ReconnectUx.NoteDropped(1000, "dropped");
		if (!service.NeedsRecoveryPump()) {
			*error = "a running recovery schedule stopped asking for its pump";
			return false;
		}
		std::cout << "PASS completed_lobby_is_not_a_recovery" << std::endl;
		return true;
	}

	// A rematch lobby only one peer came back to is destroyed when its wait runs out: the kept lease
	// is deleted once and never beaten again. A lobby seated in time keeps playing.
	bool TestCompletedLobbyExpires(std::string* error) {
		struct Wire {
			std::deque<NetDirectoryClient::Reply> replies;
			std::vector<NetDirectoryClient::Request> sent;
		};
		class ScriptedTransport final : public NetDirectoryClient::Transport {
		public:
			explicit ScriptedTransport(std::shared_ptr<Wire> wire) : m_Wire(std::move(wire)) {}
			void Start(const NetDirectoryClient::Request& request) override { m_Wire->sent.push_back(request); }
			bool Finished() override { return true; }
			NetDirectoryClient::Reply Take() override {
				// An exhausted script keeps the lease healthy, so a pump length never decides a verdict.
				// The echo repeats the visibility the request asked for: a service that contradicts it
				// is a protocol failure the client backs off from, which is not what these cases test.
				if (m_Wire->replies.empty()) {
					bool listed = true;
					if (!m_Wire->sent.empty()) {
						try {
							const nlohmann::json body = nlohmann::json::parse(m_Wire->sent.back().body);
							if (body.contains("listed")) listed = body["listed"].get<bool>();
						} catch (const nlohmann::json::exception&) {
						}
					}
					return {200, std::string(R"({"expires_in_s":15,"heartbeat_s":1,"listed":)") + (listed ? "true" : "false") + "}", ""};
				}
				NetDirectoryClient::Reply reply = m_Wire->replies.front();
				m_Wire->replies.pop_front();
				return reply;
			}
			void Abort() override {}

		private:
			std::shared_ptr<Wire> m_Wire;
		};
		struct SettingsGuard {
			std::string url = g_SettingsMan.GetSessionDirectoryUrl();
			std::string key = g_SettingsMan.GetSessionDirectoryInstallKey();
			SettingsGuard() {
				g_SettingsMan.SetSessionDirectoryUrl("https://127.0.0.1:8463");
				g_SettingsMan.SetSessionDirectoryInstallKey("key0123456789abcd");
			}
			~SettingsGuard() {
				g_SettingsMan.SetSessionDirectoryUrl(url);
				g_SettingsMan.SetSessionDirectoryInstallKey(key);
			}
		};
		struct Rig {
			LoopbackTransport host;
			LoopbackTransport client;
			NetSession clientSession;
		};
		const std::string id = "1a2b3c4d-eeee-4fff-8aaa-bbbbccccdddd";
		const std::string beat = "/v1/sessions/" + id + "/heartbeat";
		const auto fail = [&](const std::string& step) {
			*error = "completed lobby expiry: " + step;
			return false;
		};
		// Every request the directory client made, so a miss names the exchange that produced it.
		const auto trail = [](const Wire& wire) {
			std::string text;
			for (const NetDirectoryClient::Request& request : wire.sent) {
				text += (text.empty() ? "" : ", ") + request.method + " " + request.path;
			}
			return " [wire: " + text + "]";
		};
		const auto count = [](const Wire& wire, const std::string& method, const std::string& path) {
			return static_cast<size_t>(std::count_if(wire.sent.begin(), wire.sent.end(), [&](const NetDirectoryClient::Request& request) { return request.method == method && request.path == path; }));
		};
		const auto pump = [](NetMatchService& service, int times) {
			for (int i = 0; i < times; ++i) {
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
				service.Update();
			}
		};
		// A host that ran a match on a bound ICE row, with the session objects a rematch needs.
		const auto arm = [&](NetMatchService& service, Rig& rig, const std::shared_ptr<Wire>& wire, uint16_t port, std::string& step) {
			service.m_Transport = std::make_unique<GnsTransport>();
			service.m_Session = std::make_unique<NetSession>();
			if (!StartServiceRematchSession(port, rig.host, rig.client, *service.m_Session, rig.clientSession, &step)) return false;
			service.m_Runner = std::make_unique<NetMatchRunner>();
			service.m_Coordinator = std::make_unique<NetLockstepCoordinator>();
			NetMatchRunnerConfig primed;
			primed.host = true;
			primed.useLobbyProtocol = true;
			primed.sessionWaitMs = 1;
			primed.lobbyWaitMs = 30000;
			primed.lockstepWaitMs = 50;
			primed.postSessionSettleMs = 0;
			primed.postLobbySettleMs = 0;
			primed.cancelRequested = &service.m_CancelRequested;
			primed.matchConfig = NetMatchConfigUtil::MakeDefault(0x4558504952455900ULL);
			primed.matchConfig.peerCount = 2;
			LoopbackTransport idle;
			NetLockstepCoordinator unused;
			std::string primedError;
			NetSession primingSession;
			service.m_CancelRequested.store(true);
			(void)service.m_Runner->Start(idle, primingSession, unused, primed, &primedError);
			service.m_CancelRequested.store(false);
			if (!service.m_Session->IsReady()) {
				step = "runner priming replaced the seated session";
				return false;
			}
			NetLockstepConfig round;
			round.sessionId = service.m_Session->GetSessionId();
			round.localPeerId = 1;
			round.peerCount = 2;
			round.remoteTransportPeerIds = {{2, service.m_Session->GetReadyPeers().front().transportPeerId}};
			round.relayToOtherPeers = true;
			round.scenario = "completed-lobby-expiry";
			round.ownershipPolicy = "team-owner";
			if (!service.m_Coordinator->Start(rig.host, round, &step)) return false;
			service.m_Directory.SetTransportFactory([wire] { return std::make_unique<ScriptedTransport>(wire); });
			{
				std::lock_guard<std::mutex> lock(service.m_Mutex);
				service.m_IsHost = true;
				service.m_IceEnabled = true;
				service.m_State = NetMatchServiceState::Running;
			}
			service.m_BeaconGamePort = port;
			service.m_BeaconMaxPlayers = 2;
			service.m_LocalName = "ExpiryHost";
			service.m_DirectoryRow.name = "ExpiryHost";
			service.m_DirectoryRow.activity = "Skirmish Defense";
			service.m_DirectoryRow.mode = "PvP";
			service.m_DirectoryRow.peerCount = 2;
			service.m_DirectoryRow.seatsFree = 1;
			service.m_DirectoryRow.listenPort = port;
			service.m_DirectoryRow.listenAddrs = {"127.0.0.1"};
			for (int spin = 0; spin < 250 && service.m_Directory.GetState() != NetDirectoryClient::State::Registered; ++spin) {
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
				service.Update();
			}
			if (service.m_Directory.GetState() != NetDirectoryClient::State::Registered) {
				step = std::string("the directory never registered; state=") + NetDirectoryClient::StateName(service.m_Directory.GetState());
				return false;
			}
			std::lock_guard<std::mutex> lock(service.m_Mutex);
			service.m_IceBoundSessionId = service.m_Directory.GetSessionId();
			service.m_IceIdentity = NetIceHostIdentity(service.m_IceBoundSessionId);
			return true;
		};
		// Moves the lobby's wait into the past. The steady clock's own origin is the only limit.
		const auto age = [&](NetMatchService& service, std::string& step) {
			std::lock_guard<std::mutex> lock(service.m_Mutex);
			if (service.m_CompletedLobbySinceMs <= NetMatchService::c_CompletedLobbyExpiryMs) {
				step = "the steady clock is younger than the expiry, so the wait cannot be aged";
				return false;
			}
			service.m_CompletedLobbySinceMs -= NetMatchService::c_CompletedLobbyExpiryMs;
			return true;
		};
		// The roster the pump reads. The rematch worker republishes it, so each case restates its view.
		const auto publish = [](NetMatchService& service, bool remoteBack) {
			std::lock_guard<std::mutex> lock(service.m_Mutex);
			NetLobbyMember local;
			local.peerId = 1;
			local.connected = true;
			local.isLocal = true;
			NetLobbyMember remote;
			remote.peerId = 2;
			remote.connected = remoteBack;
			service.m_LobbySnapshot.members = {local, remote};
		};
		const auto registerReply = NetDirectoryClient::Reply{200, R"({"session_id":")" + id + R"(","token":"tok-expiry","expires_in_s":15,"heartbeat_s":1,"observed_ip":"127.0.0.1","supports_unlisted":true})", ""};
		SettingsGuard settings;

		{   // both peers back in time: the wait ends, the lobby stands
			auto wire = std::make_shared<Wire>();
			wire->replies = {registerReply};
			Rig rig;
			NetMatchService service;
			std::string step;
			if (arm(service, rig, wire, 48061, step)) {
				service.FinishMatch("match over");
				std::string rematchError;
				if (!service.ReturnToLobby(&rematchError)) {
					step = "ReturnToLobby refused: " + rematchError;
				}
				bool waitEnded = false;
				for (int spin = 0; step.empty() && spin < 12 && !waitEnded; ++spin) {
					publish(service, true);
					pump(service, 1);
					waitEnded = !service.NeedsCompletedLobbyPump();
				}
				if (step.empty() && !waitEnded) {
					step = "a seated rematch lobby kept its expiry wait open";
				}
				if (step.empty() && (service.GetState() == NetMatchServiceState::Idle || count(*wire, "DELETE", "/v1/sessions/" + id) != 0)) {
					step = "a seated rematch lobby was destroyed: state=" + std::string(NetMatchService::StateName(service.GetState()));
				}
			}
			service.m_CancelRequested.store(true);
			service.Destroy();
			if (!step.empty()) return fail("seated: " + step + trail(*wire));
		}

		{   // one peer only: the wait runs out, the lobby and its lease go
			auto wire = std::make_shared<Wire>();
			wire->replies = {registerReply};
			Rig rig;
			NetMatchService service;
			std::string step;
			if (arm(service, rig, wire, 48062, step)) {
				service.FinishMatch("match over");
				if (!service.NeedsCompletedLobbyPump()) {
					step = "a finished match did not open a rematch lobby wait";
				}
				std::string rematchError;
				if (step.empty() && !service.ReturnToLobby(&rematchError)) {
					step = "ReturnToLobby refused: " + rematchError;
				}
				for (int spin = 0; step.empty() && spin < 4; ++spin) {
					publish(service, false);
					pump(service, 1);
				}
				if (step.empty() && !service.NeedsCompletedLobbyPump()) {
					step = "the rematch lobby stopped waiting while a peer was still away";
				}
				if (step.empty() && count(*wire, "DELETE", "/v1/sessions/" + id) != 0) {
					step = "the lease was deleted before the wait ran out";
				}
				if (step.empty()) {
					(void)age(service, step);
				}
				if (step.empty()) {
					for (int spin = 0; spin < 4 && service.GetState() != NetMatchServiceState::Idle; ++spin) {
						publish(service, false);
						pump(service, 1);
					}
					const size_t beatsAtExpiry = count(*wire, "POST", beat);
					{
						// What the lease looked like when the expiry destroyed it.
						std::lock_guard<std::mutex> lock(service.m_Mutex);
						std::cout << "[expiry-arm] dir_state=" << NetDirectoryClient::StateName(service.m_Directory.GetState())
								  << " session=\"" << service.m_Directory.GetSessionId() << "\" bound=\"" << service.m_IceBoundSessionId
								  << "\" hidden=" << service.m_DirectoryHidden << " retracted=" << service.m_DirectoryRetracted
								  << " beats=" << beatsAtExpiry << std::endl;
					}
					if (service.GetState() != NetMatchServiceState::Idle) {
						step = "the expired lobby was not destroyed: state=" + std::string(NetMatchService::StateName(service.GetState()));
					} else if (count(*wire, "DELETE", "/v1/sessions/" + id) != 1) {
						step = "the expiry sent " + std::to_string(count(*wire, "DELETE", "/v1/sessions/" + id)) + " deletes, not 1";
					} else if (service.NeedsCompletedLobbyPump()) {
						step = "the destroyed lobby still asks the menu loop to pump it";
					} else if (service.GetErrorText() != "The rematch lobby timed out.") {
						step = "the screen was left without the expiry's reason: \"" + service.GetErrorText() + "\"";
					} else {
						pump(service, 6);
						if (count(*wire, "POST", beat) != beatsAtExpiry) {
							step = "the lease kept beating after the expiry: " + std::to_string(beatsAtExpiry) + " -> " + std::to_string(count(*wire, "POST", beat));
						} else if (count(*wire, "DELETE", "/v1/sessions/" + id) != 1) {
							step = "the expired lease was deleted more than once";
						} else {
							std::cout << "PASS completed_lobby_expires wait_s=" << NetMatchService::c_CompletedLobbyExpiryMs / 1000
									  << " deletes=1 new_beats=0" << std::endl;
						}
					}
				}
			}
			service.m_CancelRequested.store(true);
			service.Destroy();
			if (!step.empty()) return fail("one peer: " + step + trail(*wire));
		}
		return true;
	}

	bool TestServiceDirectoryIceLeaseKeepsIdentity(std::string* error) {
		struct Wire {
			std::deque<NetDirectoryClient::Reply> replies;
			std::vector<NetDirectoryClient::Request> sent;
			bool hold = false;
			std::atomic<size_t> requests{0};
		};
		class ScriptedTransport final : public NetDirectoryClient::Transport {
		public:
			explicit ScriptedTransport(std::shared_ptr<Wire> wire) : m_Wire(std::move(wire)) {}
			void Start(const NetDirectoryClient::Request& request) override { m_Wire->sent.push_back(request); ++m_Wire->requests; }
			bool Finished() override { return !m_Wire->hold; }
			NetDirectoryClient::Reply Take() override {
				if (m_Wire->replies.empty()) {
					return {500, "", ""};
				}
				NetDirectoryClient::Reply reply = m_Wire->replies.front();
				m_Wire->replies.pop_front();
				return reply;
			}
			void Abort() override {}

		private:
			std::shared_ptr<Wire> m_Wire;
		};
		struct SettingsGuard {
			std::string url = g_SettingsMan.GetSessionDirectoryUrl();
			std::string key = g_SettingsMan.GetSessionDirectoryInstallKey();
			SettingsGuard() {
				g_SettingsMan.SetSessionDirectoryUrl("https://127.0.0.1:8462");
				g_SettingsMan.SetSessionDirectoryInstallKey("key0123456789abcd");
			}
			~SettingsGuard() {
				g_SettingsMan.SetSessionDirectoryUrl(url);
				g_SettingsMan.SetSessionDirectoryInstallKey(key);
			}
		};
		struct RematchRig {
			LoopbackTransport host;
			LoopbackTransport client;
			NetSession clientSession;
		};
		const std::string idA = "7b8c9d2e-aaaa-4bbb-8ccc-ddddeeeeffff";
		const std::string idB = "8c9d2e1f-bbbb-4ccc-8ddd-eeeeffff0000";
		const std::string idLegacy = "9d2e1f30-cccc-4ddd-8eee-ffff00001111";
		const auto registerReply = [](const std::string& id, const char* token, int heartbeatS, bool capable) {
			return NetDirectoryClient::Reply{200, R"({"session_id":")" + id + R"(","token":")" + token + R"(","expires_in_s":15,"heartbeat_s":)" + std::to_string(heartbeatS) + R"(,"observed_ip":"127.0.0.1")" + (capable ? R"(,"supports_unlisted":true})" : "}"), ""};
		};
		const NetDirectoryClient::Reply hidden{200, R"({"expires_in_s":15,"heartbeat_s":1,"listed":false})", ""};
		const NetDirectoryClient::Reply listed{200, R"({"expires_in_s":15,"heartbeat_s":1,"listed":true})", ""};
		const NetDirectoryClient::Reply deleted{200, R"({"ok":true})", ""};
		const NetDirectoryClient::Reply gone{404, R"({"error":"not_found"})", ""};
		const auto count = [](const Wire& wire, const std::string& method, const std::string& path) {
			return static_cast<size_t>(std::count_if(wire.sent.begin(), wire.sent.end(), [&](const NetDirectoryClient::Request& request) { return request.method == method && request.path == path; }));
		};
		const auto body = [](const NetDirectoryClient::Request& request) {
			try {
				return nlohmann::json::parse(request.body);
			} catch (const nlohmann::json::exception&) {
				return nlohmann::json();
			}
		};
		const auto update = [](NetMatchService& service) {
			{
				std::lock_guard<std::mutex> lock(service.m_Mutex);
				if (service.m_State == NetMatchServiceState::Starting) service.m_State = NetMatchServiceState::ReadyToLaunch;
			}
			service.Update();
		};
		const auto pumpUntil = [&](NetMatchService& service, uint64_t budgetMs, const std::function<bool()>& done) {
			const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
			while (!done() && std::chrono::steady_clock::now() < until) {
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
				update(service);
			}
			return done();
		};
		const auto pump = [&](NetMatchService& service, int times) {
			for (int i = 0; i < times; ++i) {
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
				update(service);
			}
		};
		const auto confirmed = [](NetMatchService& service) {
			return nlohmann::json::parse(service.m_Directory.BuildReportJson()).value("confirmed_listed", nlohmann::json());
		};
		// A running host registers without the lobby's LAN beacon, then pins its identity as SetUpIceTransport does.
		const auto hostAndBind = [&](NetMatchService& service, bool ice, const std::shared_ptr<Wire>& wire, std::string& step) {
			service.m_Directory.SetTransportFactory([wire] { return std::make_unique<ScriptedTransport>(wire); });
			{
				std::lock_guard<std::mutex> lock(service.m_Mutex);
				service.m_IsHost = true;
				service.m_IceEnabled = ice;
				service.m_State = NetMatchServiceState::Running;
			}
			service.m_BeaconGamePort = 48041;
			service.m_BeaconMaxPlayers = 2;
			service.m_LocalName = "LeaseHost";
			service.m_DirectoryRow.name = "LeaseHost";
			service.m_DirectoryRow.activity = "Skirmish Defense";
			service.m_DirectoryRow.mode = "PvP";
			service.m_DirectoryRow.peerCount = 2;
			service.m_DirectoryRow.seatsFree = 1;
			service.m_DirectoryRow.listenPort = 48041;
			service.m_DirectoryRow.listenAddrs = {"127.0.0.1"};
			if (!pumpUntil(service, 500, [&] { return service.m_Directory.GetState() == NetDirectoryClient::State::Registered; })) {
				step = std::string("the directory never registered; state=") + NetDirectoryClient::StateName(service.m_Directory.GetState());
				return false;
			}
			if (ice) {
				std::lock_guard<std::mutex> lock(service.m_Mutex);
				service.m_IceBoundSessionId = service.m_Directory.GetSessionId();
				service.m_IceIdentity = NetIceHostIdentity(service.m_IceBoundSessionId);
			}
			return true;
		};
		// ReturnToLobby needs a live session and a runner; the rematch lobby then waits until Destroy cancels it.
		const auto armRematch = [](NetMatchService& service, RematchRig& rig, uint16_t port, std::string& step) {
			service.m_Transport = std::make_unique<GnsTransport>();
			service.m_Session = std::make_unique<NetSession>();
			if (!StartServiceRematchSession(port, rig.host, rig.client, *service.m_Session, rig.clientSession, &step)) return false;
			service.m_Runner = std::make_unique<NetMatchRunner>();
			service.m_Coordinator = std::make_unique<NetLockstepCoordinator>();
			NetMatchRunnerConfig primed;
			primed.host = true;
			primed.useLobbyProtocol = true;
			primed.sessionWaitMs = 1;
			primed.lobbyWaitMs = 30000;
			primed.lockstepWaitMs = 50;
			primed.postSessionSettleMs = 0;
			primed.postLobbySettleMs = 0;
			primed.cancelRequested = &service.m_CancelRequested;
			primed.matchConfig = NetMatchConfigUtil::MakeDefault(0x4449524C45415345ULL);
			primed.matchConfig.peerCount = 2;
			LoopbackTransport idle;
			NetLockstepCoordinator unused;
			std::string primedError;
			NetSession primingSession;
			// Prime the config without replacing the seated session.
			service.m_CancelRequested.store(true);
			(void)service.m_Runner->Start(idle, primingSession, unused, primed, &primedError);
			service.m_CancelRequested.store(false);
			if (!service.m_Session->IsReady()) {
				step = "runner priming replaced the seated session";
				return false;
			}
			// The completed pump recognizes peers from the round's real transport map.
			NetLockstepConfig round;
			round.sessionId = service.m_Session->GetSessionId();
			round.localPeerId = 1;
			round.peerCount = 2;
			round.remoteTransportPeerIds = {{2, service.m_Session->GetReadyPeers().front().transportPeerId}};
			round.relayToOtherPeers = true;
			round.scenario = "directory-lease";
			round.ownershipPolicy = "team-owner";
			return service.m_Coordinator->Start(rig.host, round, &step);
		};
		const auto finish = [](NetMatchService& service) {
			service.m_CancelRequested.store(true);
			service.Destroy();
		};
		std::vector<std::string> misses;
		SettingsGuard settings;

		{   // a natural ICE end hides the bound row, keeps beating it, and the rematch relists that same row
			auto wire = std::make_shared<Wire>();
			wire->replies = {registerReply(idA, "tok-a", 1, true), hidden, hidden, listed, deleted};
			RematchRig rig;
			NetMatchService service;
			std::string step;
			const std::string beat = "/v1/sessions/" + idA + "/heartbeat";
			const auto beatSays = [&](size_t n, bool wantListed) {
				size_t seen = 0;
				for (const NetDirectoryClient::Request& request : wire->sent) {
					if (request.path == beat && ++seen == n) {
						const nlohmann::json sentBody = body(request);
						return sentBody.value("listed", nlohmann::json()) == nlohmann::json(wantListed) && sentBody.value("token", "") == "tok-a";
					}
				}
				return false;
			};
			if (armRematch(service, rig, 48042, step) && hostAndBind(service, true, wire, step)) {
				service.Complete("e2e complete");
				pump(service, 3);
				if (service.GetState() != NetMatchServiceState::Running || count(*wire, "DELETE", "/v1/sessions/" + idA) != 0 || count(*wire, "POST", beat) != 1 || !beatSays(1, false) || confirmed(service) != nlohmann::json(false)) {
					step = "Complete did not hide the bound row on its own heartbeat; deletes=" + std::to_string(count(*wire, "DELETE", "/v1/sessions/" + idA));
				}
				service.FinishMatch("match over");
				if (step.empty() && !pumpUntil(service, 2500, [&] { return count(*wire, "POST", beat) >= 2; })) {
					step = "the hidden row sent no interval keepalive";
				}
				pump(service, 3);
				if (step.empty() && (service.GetState() != NetMatchServiceState::Completed || !beatSays(2, false) || service.m_LanDiscovery.IsBeaconing() || count(*wire, "POST", "/v1/sessions") != 1 || count(*wire, "DELETE", "/v1/sessions/" + idA) != 0)) {
					step = "the completed ICE host did not keep its row hidden, beating and beacon-free";
				}
				std::string rematchError;
				if (step.empty() && !service.ReturnToLobby(&rematchError)) {
					step = "ReturnToLobby refused: " + rematchError;
				}
				if (step.empty() && !pumpUntil(service, 1000, [&] { return confirmed(service) == nlohmann::json(true); })) {
					step = "the rematch lobby never relisted the kept row";
				}
				if (step.empty() && (count(*wire, "POST", beat) != 3 || !beatSays(3, true) || count(*wire, "POST", "/v1/sessions") != 1 || service.m_Directory.GetSessionId() != idA || service.m_Directory.GetToken() != "tok-a")) {
					step = "the relist changed identity: registers=" + std::to_string(count(*wire, "POST", "/v1/sessions")) + " heartbeats=" + std::to_string(count(*wire, "POST", beat));
				}
				if (step.empty()) {
					NetDirectorySessionRow rematchRow;
					rematchRow.sessionId = service.m_Directory.GetSessionId();
					rematchRow.peerCount = service.m_DirectoryRow.peerCount;
					rematchRow.seatsFree = service.m_DirectoryRow.seatsFree;
					rematchRow.listenPort = service.m_DirectoryRow.listenPort;
					rematchRow.listenAddrs = service.m_DirectoryRow.listenAddrs;
					rematchRow.joinMode = service.m_DirectoryRow.joinMode;
					NetIceJoinTarget laterJoin;
					const std::string refusal = NetIceResolveSessionRow({rematchRow}, {}, rematchRow.sessionId, &laterJoin);
					if (!refusal.empty() || laterJoin.identity != service.m_IceIdentity || laterJoin.joinMode != "either") {
						step = "later rematch join resolved " + laterJoin.identity + " via " + laterJoin.joinMode + " (" + refusal + ")";
					} else {
						std::cout << "PASS ice_rematch_row_resolves_bound_identity " << laterJoin.identity << std::endl;
					}
				}
			}
			finish(service);
			if (step.empty() && count(*wire, "DELETE", "/v1/sessions/" + idA) != 1) {
				step = "Destroy did not delete the kept row";
			}
			if (!step.empty()) misses.push_back("ice end: " + step);
		}

		{   // a direct-IP end deletes as before, and its rematch lobby registers afresh
			auto wire = std::make_shared<Wire>();
			wire->replies = {registerReply(idLegacy, "tok-ip", 60, false), deleted, registerReply(idB, "tok-ip-2", 60, false)};
			RematchRig rig;
			NetMatchService service;
			std::string step;
			if (armRematch(service, rig, 48043, step) && hostAndBind(service, false, wire, step)) {
				service.FinishMatch("match over");
				if (!pumpUntil(service, 500, [&] { return count(*wire, "DELETE", "/v1/sessions/" + idLegacy) == 1 && service.m_Directory.GetState() == NetDirectoryClient::State::Idle; })) {
					step = "FinishMatch did not delete the IP row";
				}
				std::string rematchError;
				if (step.empty() && !service.ReturnToLobby(&rematchError)) {
					step = "ReturnToLobby refused: " + rematchError;
				}
				if (step.empty() && !pumpUntil(service, 500, [&] { return count(*wire, "POST", "/v1/sessions") == 2; })) {
					step = "the IP rematch lobby did not register again";
				}
			}
			finish(service);
			if (!step.empty()) misses.push_back("ip end: " + step);
		}

		{   // leaving an ICE match deletes the bound row
			auto wire = std::make_shared<Wire>();
			wire->replies = {registerReply(idA, "tok-a", 60, true), deleted};
			NetMatchService service;
			std::string step;
			if (hostAndBind(service, true, wire, step)) {
				service.LeaveMatch("player left");
				if (!pumpUntil(service, 500, [&] { return count(*wire, "DELETE", "/v1/sessions/" + idA) == 1; })) {
					step = "LeaveMatch kept the row";
				}
			}
			finish(service);
			if (!step.empty()) misses.push_back("ice leave: " + step);
		}

		{   // legacy service: the hide's delete is still out when the rematch starts; nothing registers again
			auto wire = std::make_shared<Wire>();
			wire->replies = {registerReply(idLegacy, "tok-legacy", 60, false), deleted, registerReply(idB, "tok-legacy-2", 60, false)};
			RematchRig rig;
			NetMatchService service;
			std::string step;
			if (armRematch(service, rig, 48044, step) && hostAndBind(service, true, wire, step)) {
				wire->hold = true;
				service.FinishMatch("match over");
				std::string rematchError;
				if (count(*wire, "DELETE", "/v1/sessions/" + idLegacy) != 1) {
					step = "the unsupported hide did not delete the row";
				} else if (!service.ReturnToLobby(&rematchError)) {
					step = "ReturnToLobby refused: " + rematchError;
				}
				pump(service, 3);
				wire->hold = false;
				pump(service, 20);
				if (step.empty() && count(*wire, "POST", "/v1/sessions") != 1) {
					step = "a replacement row was registered: registers=" + std::to_string(count(*wire, "POST", "/v1/sessions"));
				}
			}
			finish(service);
			if (!step.empty()) misses.push_back("legacy in flight: " + step);
		}

		{   // hidden 404: the hide's 404 is still out when the rematch starts; the lost row is deleted, never replaced
			auto wire = std::make_shared<Wire>();
			wire->replies = {registerReply(idA, "tok-a", 60, true), gone, deleted, registerReply(idB, "tok-b", 60, true)};
			RematchRig rig;
			NetMatchService service;
			std::string step;
			if (armRematch(service, rig, 48045, step) && hostAndBind(service, true, wire, step)) {
				wire->hold = true;
				service.FinishMatch("match over");
				std::string rematchError;
				if (!service.ReturnToLobby(&rematchError)) {
					step = "ReturnToLobby refused: " + rematchError;
				}
				pump(service, 3);
				wire->hold = false;
				(void)pumpUntil(service, 500, [&] { return count(*wire, "DELETE", "/v1/sessions/" + idA) == 1; });
				pump(service, 10);
				if (step.empty() && (count(*wire, "POST", "/v1/sessions") != 1 || count(*wire, "DELETE", "/v1/sessions/" + idA) != 1)) {
					step = "registers=" + std::to_string(count(*wire, "POST", "/v1/sessions")) + " deletes=" + std::to_string(count(*wire, "DELETE", "/v1/sessions/" + idA));
				}
			}
			finish(service);
			if (!step.empty()) misses.push_back("hidden 404 in flight: " + step);
		}

		{   // a mid-match re-register is not the bound row: its register says ip, and the end deletes it
			auto wire = std::make_shared<Wire>();
			wire->replies = {registerReply(idA, "tok-a", 1, true), gone, registerReply(idB, "tok-b", 60, true), deleted};
			NetMatchService service;
			std::string step;
			if (hostAndBind(service, true, wire, step)) {
				if (!pumpUntil(service, 2500, [&] { return service.m_Directory.GetSessionId() == idB; })) {
					step = "the visible 404 never re-registered";
				} else {
					const std::string first = body(wire->sent.at(0)).value("join_mode", "");
					const std::string second = body(wire->sent.at(2)).value("join_mode", "");
					service.FinishMatch("match over");
					if (!pumpUntil(service, 500, [&] { return count(*wire, "DELETE", "/v1/sessions/" + idB) == 1; })) {
						step = "the end kept a row GNS is not pinned to";
					}
					if (first != "either" || second != "ip") {
						step += (step.empty() ? "" : "; ") + std::string("register join_mode ") + first + " then " + second + ", want either then ip";
					}
				}
			}
			finish(service);
			if (!step.empty()) misses.push_back("rebound row: " + step);
		}



		// A held hide must settle before the relist changes the requested visibility.
		{
			auto wire = std::make_shared<Wire>();
			wire->replies = {registerReply(idA, "tok-a", 60, true), hidden, listed, deleted};
			RematchRig rig;
			NetMatchService service;
			std::string step;
			if (armRematch(service, rig, 48046, step) && hostAndBind(service, true, wire, step)) {
				wire->hold = true;
				service.FinishMatch("match over");
				if (!service.ReturnToLobby(&step) && step.empty()) step = "ReturnToLobby refused";
				pump(service, 3);
				const bool desired = nlohmann::json::parse(service.m_Directory.BuildReportJson()).value("desired_listed", true);
				const size_t earlyDeletes = count(*wire, "DELETE", "/v1/sessions/" + idA);
				wire->hold = false;
				pump(service, 8);
				const size_t registers = count(*wire, "POST", "/v1/sessions");
				const size_t beats = count(*wire, "POST", "/v1/sessions/" + idA + "/heartbeat");
				std::cout << "[net-match-selftest] MEASURE directory lease delayed hide desired_before_ack=" << desired << " early_deletes=" << earlyDeletes << " registers=" << registers << " heartbeats=" << beats << std::endl;
				if (step.empty() && (desired || earlyDeletes != 0 || registers != 1 || beats != 2 || confirmed(service) != nlohmann::json(true) || service.m_Directory.GetSessionId() != idA)) {
					step = "desired_before_ack=" + std::to_string(desired) + " early_deletes=" + std::to_string(earlyDeletes) + " registers=" + std::to_string(registers) + " heartbeats=" + std::to_string(beats);
				}
			}
			finish(service);
			if (!step.empty()) misses.push_back("delayed hide: " + step);
		}

		// Failure can settle while Complete still leaves the service Running.
		for (bool capable : {false, true}) {
			auto wire = std::make_shared<Wire>();
			wire->replies = {registerReply(idA, "tok-a", 60, capable), capable ? gone : deleted, deleted, registerReply(idB, "tok-b", 60, true)};
			RematchRig rig;
			NetMatchService service;
			std::string step;
			const std::string label = capable ? "settled hidden 404" : "settled legacy";
			if (armRematch(service, rig, capable ? 48048 : 48047, step) && hostAndBind(service, true, wire, step)) {
				service.Complete("match over");
				pump(service, 8);
				service.FinishMatch("match over");
				pump(service, 3);
				if (!service.ReturnToLobby(&step) && step.empty()) step = "ReturnToLobby refused";
				pump(service, 10);
				const size_t registers = count(*wire, "POST", "/v1/sessions");
				const size_t deletes = count(*wire, "DELETE", "/v1/sessions/" + idA);
				std::cout << "[net-match-selftest] MEASURE directory lease " << label << " registers=" << registers << " deletes=" << deletes << std::endl;
				if (step.empty() && (registers != 1 || deletes != 1)) {
					step = "registers=" + std::to_string(registers) + " deletes=" + std::to_string(deletes);
				}
			}
			finish(service);
			if (!step.empty()) misses.push_back(label + ": " + step);
		}

		// A lease decision waits for the worker's shared state before touching the directory.
		{
			auto wire = std::make_shared<Wire>();
			wire->replies = {registerReply(idA, "tok-a", 60, true), deleted};
			NetMatchService service;
			std::string step;
			if (hostAndBind(service, true, wire, step)) {
				const size_t before = wire->requests.load();
				std::atomic<bool> entering{false};
				std::unique_lock<std::mutex> lock(service.m_Mutex);
				std::thread complete([&] {
					entering.store(true);
					service.Complete("match over");
				});
				while (!entering.load()) std::this_thread::yield();
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				const size_t whileLocked = wire->requests.load() - before;
				service.m_IceBoundSessionId.clear();
				lock.unlock();
				complete.join();
				const size_t deletes = count(*wire, "DELETE", "/v1/sessions/" + idA);
				std::cout << "[net-match-selftest] MEASURE directory lease locked snapshot requests_while_locked=" << whileLocked << " deletes=" << deletes << std::endl;
				if (whileLocked != 0 || deletes != 1) {
					step = "requests_while_locked=" + std::to_string(whileLocked) + " deletes_after_unbind=" + std::to_string(deletes);
				}
			}
			finish(service);
			if (!step.empty()) misses.push_back("locked snapshot: " + step);
		}

		if (!misses.empty()) {
			*error = "directory lease misses (" + std::to_string(misses.size()) + "):";
			for (const std::string& miss : misses) {
				*error += " [" + miss + "]";
			}
			return false;
		}
		std::cout << "PASS service_directory_lease ice_end_hides_keeps_relists=1 ip_leave_delete=1 legacy_404_in_flight_no_reregister=1 rebound_row_deleted_ip=1" << std::endl;
		return true;
	}

	bool TestIceRowJoinMode(std::string* error) {
		struct Case {
			bool enabled;
			bool direct;
			const char* bound;
			const char* row;
			const char* want;
			const char* what;
		};
		const Case cases[] = {
			{false, true, "", "s1", "ip", "ICE off"},
			{false, false, "", "s1", "ip", "ICE off without an address"},
			{true, true, "", "", "either", "ICE on before the register reply"},
			{true, false, "", "", "ice", "ICE on with no direct address"},
			{true, true, "s1", "s1", "either", "the bound session id"},
			{true, false, "s1", "s1", "ice", "the bound session id, ICE only"},
			{true, true, "s1", "s2", "ip", "a rematch under a new session id"},
			{true, false, "s1", "s2", "ip", "a rematch with no direct address"},
			// A register after the bound row went away mints a new id the pinned identity cannot answer.
			{true, true, "s1", "", "ip", "a re-register of a bound host"},
			{true, false, "s1", "", "ip", "a re-register of a bound host with no direct address"},
		};
		std::string mismatches;
		for (const Case& c: cases) {
			const std::string got = NetIceRowJoinMode(c.enabled, c.direct, c.bound, c.row);
			if (got != c.want) {
				mismatches += std::string(mismatches.empty() ? "" : "; ") + c.what + " gave \"" + got + "\", expected \"" + c.want + "\"";
			}
		}
		if (!mismatches.empty()) {
			*error = "ice join_mode: " + mismatches;
			return false;
		}
		std::cout << "[net-match-selftest] PASS ice join_mode: the bound row remains ICE-capable; replacement ids advertise ip" << std::endl;
		return true;
	}

	bool TestSessionIdJoinRefusals(std::string* error) {
		NetDirectoryLocalIdentity local;
		local.networkProtocolVersion = 1;
		local.lockstepCodecVersion = 20;
		local.controllerFrameVersion = 6;
		local.sessionIdentityHash = std::string(64, 'b');
		local.moduleManifestHash = std::string(64, 'c');

		auto sample = [&local](const std::string& id) {
			NetDirectorySessionRow row;
			row.name = "Erol";
			row.peerCount = 2;
			row.seatsFree = 1;
			row.networkProtocolVersion = local.networkProtocolVersion;
			row.lockstepCodecVersion = local.lockstepCodecVersion;
			row.controllerFrameVersion = local.controllerFrameVersion;
			row.sessionIdentityHash = local.sessionIdentityHash;
			row.moduleManifestHash = local.moduleManifestHash;
			row.joinMode = "ice";
			row.sessionId = id;
			return row;
		};

		NetDirectorySessionRow ok = sample("live");
		NetDirectorySessionRow full = sample("full");
		full.seatsFree = 0;
		NetDirectorySessionRow codec = sample("codec");
		codec.lockstepCodecVersion = 99;
		NetDirectorySessionRow ipOnly = sample("iponly");
		ipOnly.joinMode = "ip";
		NetDirectorySessionRow eitherRow = sample("either");
		eitherRow.joinMode = "either";
		eitherRow.listenAddrs = {"127.0.0.1"};
		eitherRow.listenPort = 41010;
		const std::vector<NetDirectorySessionRow> rows = {ok, full, codec, ipOnly, eitherRow};

		struct Case {
			const char* id;
			const char* reason;
		};
		const Case refusals[] = {{"absent", "no such session"}, {"full", "full"}, {"codec", "codec"}, {"iponly", "address"}};
		for (const Case& c: refusals) {
			NetIceJoinTarget target;
			const std::string why = NetIceResolveSessionRow(rows, local, c.id, &target);
			if (why != c.reason) {
				*error = std::string("session-id join: \"") + c.id + "\" was refused with \"" + why + "\", expected \"" + c.reason + "\"";
				return false;
			}
		}
		NetIceJoinTarget target;
		if (!NetIceResolveSessionRow(rows, local, "live", &target).empty() || target.identity != "str:h-live" || target.joinMode != "ice") {
			*error = "session-id join: the joinable ice row did not resolve to its host identity";
			return false;
		}
#ifdef CCCP_WITH_GNS
		// One rule for the identity: the resolver's copy must not drift from the dispatcher's.
		const std::string sampleId = "7B8C9D2E-1111-4222-8333-4444555566667777";
		if (NetIceHostIdentity(sampleId) != GnsDirectorySignalDispatcher::HostIdentity(sampleId)) {
			*error = "session-id join: NetIceHostIdentity and GnsDirectorySignalDispatcher::HostIdentity disagree on " + sampleId;
			return false;
		}
#endif
		NetIceJoinTarget either;
		if (!NetIceResolveSessionRow(rows, local, "either", &either).empty() || either.address != "127.0.0.1" || either.port != 41010) {
			*error = "session-id join: an either row did not carry its direct address too";
			return false;
		}
		std::cout << "[net-match-selftest] PASS session-id join: an absent, full, mismatched or ip-only row is refused with the join list's own label; an ice row resolves to str:h-<session>, an either row keeps its address" << std::endl;
		return true;
	}

	// -net-ice is a run override: it decides this run and never reaches the saved settings.
	bool TestIceSettingsOverrideIsNotPersisted(std::string* error) {
		// This selftest runs before the managers are built.
		if (!SettingsMan::IsConstructed()) SettingsMan::Construct();
		const bool savedEnable = g_SettingsMan.GetNetworkIceEnableSetting();
		const std::string savedStun = g_SettingsMan.GetNetworkStunServers();
		g_SettingsMan.SetNetworkIceEnable(false);
		g_SettingsMan.SetNetworkStunServers("");
		if (g_SettingsMan.GetNetworkIceEnable()) {
			*error = "ice settings: the default was not off";
			return false;
		}
		g_SettingsMan.SetNetworkIceEnableOverride(true);
		if (!g_SettingsMan.GetNetworkIceEnable() || g_SettingsMan.GetNetworkIceEnableSetting()) {
			*error = "ice settings: the -net-ice override did not decide the run, or it reached the saved setting";
			return false;
		}
		g_SettingsMan.SetNetworkStunServersOverride("stun.example:3478");
		if (g_SettingsMan.GetNetworkStunServers() != "stun.example:3478" || !g_SettingsMan.GetNetworkStunServersSetting().empty()) {
			*error = "ice settings: the -net-stun override did not decide the run, or it reached the saved setting";
			return false;
		}
		g_SettingsMan.ClearNetworkIceOverrides();
		if (g_SettingsMan.GetNetworkIceEnable() || !g_SettingsMan.GetNetworkStunServers().empty()) {
			*error = "ice settings: clearing the overrides did not fall back to the saved settings";
			return false;
		}
		g_SettingsMan.SetNetworkIceEnable(savedEnable);
		g_SettingsMan.SetNetworkStunServers(savedStun);
		std::cout << "[net-match-selftest] PASS ice settings: NetworkIceEnable defaults off; -net-ice and -net-stun decide the run and never touch the saved value" << std::endl;
		return true;
	}

	// The runner's SessionFull retry calls StartClient again, so the join spec has to ride the config.
	bool TestP2PJoinSpecRidesTheSessionConfig(std::string* error) {
		std::vector<std::string> log;
		TransportTap tap("tap", &log);
		int dialled = 0;

		NetSessionConfig config;
		config.displayName = "Client";
		config.p2pJoin.identity = "str:h-abc";
		config.p2pJoin.remoteVirtualPort = 41011;
		config.p2pJoin.connect = [&dialled](INetTransport&, std::string*) {
			++dialled;
			return true;
		};

		NetSession session;
		std::string startError;
		if (!session.StartClientP2P(tap, config, &startError)) {
			*error = "p2p join spec: StartClientP2P failed: " + startError;
			return false;
		}
		if (dialled != 1 || !log.empty()) {
			*error = "p2p join spec: the session dialled the address instead of the spec";
			return false;
		}
		if (session.GetRole() != NetSessionRole::Client || session.GetState() != NetSessionState::Connecting) {
			*error = "p2p join spec: the session did not enter the client connecting state";
			return false;
		}
		// Exactly what the runner's retry does, with the config it kept.
		NetSessionConfig retry = config;
		if (!session.StartClient(tap, "203.0.113.9", retry, &startError)) {
			*error = "p2p join spec: the retry failed: " + startError;
			return false;
		}
		if (dialled != 2 || !log.empty()) {
			*error = "p2p join spec: the retry dialled the address instead of replaying the spec";
			return false;
		}
		NetSessionConfig plain;
		NetSession ip;
		if (!ip.StartClient(tap, "203.0.113.9", plain, &startError) || log.size() != 1 || log[0] != "tap.Connect(203.0.113.9:41010)") {
			*error = "p2p join spec: a config without a spec no longer takes the direct-IP path";
			return false;
		}
		std::cout << "[net-match-selftest] PASS p2p join spec: StartClient dials the config's spec and replays it on the SessionFull retry; without a spec the direct-IP Connect is unchanged" << std::endl;
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
		if (!TestCPURosterRequests(&error)) return fail(error);
		if (!TestCPURosterValidation(&error)) return fail(error);
		if (!TestCPURosterHash(&error)) return fail(error);
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
		if (!TestLobbyStartsWithoutRemoteHumanSeats(&error)) return fail(error);
		if (!TestAiOnlyHostSeatsNoJoiner(&error)) return fail(error);
		if (!TestLobbyStateTransfer(&error)) return fail(error);
		if (!TestLobbyStateChunkBounds(&error)) return fail(error);
		if (!TestLobbyStateChunkConsistency(&error)) return fail(error);
		if (!TestLobbyStateTransferRestart(&error)) return fail(error);
		if (!TestLobbyStateTransferBackpressure(&error)) return fail(error);
		if (!TestRunnerStateTransferProgress(&error)) return fail(error);
		if (!TestRematchRosterDerivation(&error)) return fail(error);
		if (!TestRematchRebuildsTheSurvivingRoster(&error)) return fail(error);
		if (!TestRematchProposalFits(&error)) return fail(error);
		// The ICE lifecycle arms fail at the end, so one red arm cannot hide another.
		std::string serviceRosterError;
		if (!TestServiceReturnToLobbyFormsTheNextRoster(&serviceRosterError)) {
			std::cerr << "[net-match-selftest] FAIL: " << serviceRosterError << std::endl;
		}
		// Each rematch-roster case reports its own verdict, so one red case cannot hide another.
		std::string twoShrinksError;
		if (!TestRematchKeepsStableSeatsAcrossTwoShrinks(&twoShrinksError)) {
			std::cerr << "[net-match-selftest] FAIL: " << twoShrinksError << std::endl;
		}
		std::string hardDropError;
		if (!TestRematchAfterAHardDrop(&hardDropError)) {
			std::cerr << "[net-match-selftest] FAIL: " << hardDropError << std::endl;
		}
		std::string reclaimError;
		if (!TestRematchRoundReclaimsAfterAShrink(&reclaimError)) {
			std::cerr << "[net-match-selftest] FAIL: " << reclaimError << std::endl;
		}
		if (!twoShrinksError.empty()) return fail(twoShrinksError);
		if (!hardDropError.empty()) return fail(hardDropError);
		if (!reclaimError.empty()) return fail(reclaimError);
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
		if (!tickClockError.empty()) return fail(tickClockError);
		if (!capTickError.empty()) return fail(capTickError);
		if (!executedTickError.empty()) return fail(executedTickError);
		if (!earlyOverTickError.empty()) return fail(earlyOverTickError);
		if (!healedEndError.empty()) return fail(healedEndError);
		if (!TestHoldResolutionPumpDoesNotRelock(&error)) return fail(error);
		if (!TestRosterTransitionsRecordHoldThenPresent(&error)) return fail(error);
		if (!TestRosterBannerNamesThePlayerOnce(&error)) return fail(error);
		// The chat arms accumulate like the other independent tests so one defective build shows
		// every defect instead of stopping at the first.
		std::string chatRoutingError, chatRaceError, chatCarryError;
		if (!TestChatRoutingAndBounds(&chatRoutingError)) {
			std::cerr << "[net-match-selftest] FAIL: " << chatRoutingError << std::endl;
		}
		if (!TestChatOutboxJoinRace(&chatRaceError)) {
			std::cerr << "[net-match-selftest] FAIL: " << chatRaceError << std::endl;
		}
		if (!TestChatSendRefusedOutsideCarry(&chatCarryError)) {
			std::cerr << "[net-match-selftest] FAIL: " << chatCarryError << std::endl;
		}
		if (!chatRoutingError.empty()) return fail(chatRoutingError);
		if (!chatRaceError.empty()) return fail(chatRaceError);
		if (!chatCarryError.empty()) return fail(chatCarryError);
		if (!TestPendingSessionEventSurvivesTeardown(&error)) return fail(error);
		if (!TestFinishMatchDrainsFencedDisconnect(&error)) return fail(error);
		std::string stopCancelError, endedAdmissionError, twoIceRoundsError;
		if (!TestServiceIceRematchPlaysTwoRounds(&twoIceRoundsError)) std::cerr << "[net-match-selftest] FAIL: " << twoIceRoundsError << std::endl;
		if (!TestGnsStopCancelContracts(&stopCancelError)) std::cerr << "[net-match-selftest] FAIL: " << stopCancelError << std::endl;
		if (!TestEndedWorldLateAdmission(&endedAdmissionError)) std::cerr << "[net-match-selftest] FAIL: " << endedAdmissionError << std::endl;
		if (!TestMuxOpensIceListenFirst(&error)) return fail(error);
		if (!TestMuxRoutesByTag(&error)) return fail(error);
		std::string iceRowError;
		if (!TestIceRowJoinMode(&iceRowError)) {
			std::cerr << "[net-match-selftest] FAIL: " << iceRowError << std::endl;
		}
		if (!TestSessionIdJoinRefusals(&error)) return fail(error);
		if (!TestIceSettingsOverrideIsNotPersisted(&error)) return fail(error);
		if (!TestP2PJoinSpecRidesTheSessionConfig(&error)) return fail(error);
		if (!TestServiceDirectoryIceLeaseKeepsIdentity(&error)) return fail(error);
		if (!TestCompletedLobbyIsNotARecovery(&error)) return fail(error);
		if (!TestCompletedLobbyExpires(&error)) return fail(error);
		if (!twoIceRoundsError.empty()) return fail(twoIceRoundsError);
		if (!stopCancelError.empty()) return fail(stopCancelError);
		if (!endedAdmissionError.empty()) return fail(endedAdmissionError);
		if (!serviceRosterError.empty()) return fail(serviceRosterError);
		if (!rejoinWaitError.empty()) return fail(rejoinWaitError);
		if (!iceRowError.empty()) return fail(iceRowError);

		std::cout << "[net-match-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
