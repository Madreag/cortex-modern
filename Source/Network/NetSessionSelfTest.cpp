#include "NetSessionSelfTest.h"

#include "ControllerFrame.h"
#include "LoopbackTransport.h"
#include "NetLobbySession.h"
#include "NetSession.h"

#include <algorithm>
#include <array>
#include <functional>
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

		NetIdentityManifest MakeIdentity() {
			NetIdentityManifest manifest;
			manifest.gameVersion = "7.0.0-test";
			manifest.networkProtocolVersion = NetProtocol::c_Version;
			manifest.controllerFrameVersion = ControllerFrame::c_Version;
			manifest.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
			manifest.buildId = "stage2-p2c-selftest";
			manifest.platform = "test";
			manifest.deterministicConfigHash = MakeHash(1);
			manifest.moduleManifestHash = MakeHash(33);
			manifest.sessionRulesHash = MakeHash(65);
			manifest.sessionIdentityHash = MakeHash(97);
			manifest.hasUserdataModules = false;
			return manifest;
		}

		NetSessionConfig MakeConfig(uint16_t port, uint64_t nonce, const std::string& name = "Player") {
			NetSessionConfig config;
			config.localIdentity = MakeIdentity();
			config.displayName = name;
			config.port = port;
			config.sessionId = 0x5000000000000000ULL + port;
			config.localNonce = nonce;
			config.maxPeers = 1;
			config.heartbeatIntervalMs = 50;
			config.timeoutMs = 250;
			return config;
		}

		NetClientHello MakeClientHello(const NetSessionConfig& config) {
			NetClientHello hello;
			hello.clientNonce = config.localNonce;
			hello.minProtocolVersion = config.minProtocolVersion;
			hello.maxProtocolVersion = config.maxProtocolVersion;
			hello.controllerFrameVersion = config.localIdentity.controllerFrameVersion;
			hello.controllerFrameEncodedSize = config.localIdentity.controllerFrameEncodedSize;
			hello.displayName = config.displayName;
			hello.gameVersion = config.localIdentity.gameVersion;
			hello.buildId = config.localIdentity.buildId;
			hello.deterministicConfigHash = config.localIdentity.deterministicConfigHash;
			hello.moduleManifestHash = config.localIdentity.moduleManifestHash;
			hello.sessionRulesHash = config.localIdentity.sessionRulesHash;
			hello.sessionIdentityHash = config.localIdentity.sessionIdentityHash;
			hello.hasUserdataModules = config.localIdentity.hasUserdataModules;
			return hello;
		}

		class ScriptedHostTransport final : public INetTransport {
		public:
			struct SentPacket {
				NetPeerId peerId = c_InvalidNetPeerId;
				std::vector<uint8_t> bytes;
			};

			bool StartHost(uint16_t, std::string*) override { return true; }
			bool Connect(const std::string&, uint16_t, std::string* error) override {
				if (error) *error = "scripted transport does not connect";
				return false;
			}
			bool Send(NetPeerId peerId, NetTransportLane, const std::vector<uint8_t>& bytes, std::string*, bool*) override {
				sentPackets.push_back({peerId, bytes});
				return true;
			}
			void Disconnect(NetPeerId, const std::string&) override {}
			void Stop() override { events.clear(); }
			std::vector<NetTransportEvent> PollEvents() override {
				std::vector<NetTransportEvent> result = std::move(events);
				events.clear();
				return result;
			}
			void Push(NetTransportEvent event) { events.push_back(std::move(event)); }

			std::vector<SentPacket> sentPackets;

		private:
			std::vector<NetTransportEvent> events;
		};

		bool DrivePair(LoopbackTransport& hostTransport, LoopbackTransport& clientTransport, NetSession& host, NetSession& client, const std::function<bool()>& done, std::string* error, uint64_t maxMs = 1000, uint64_t stepMs = 10) {
			for (uint64_t now = 0; now <= maxMs; now += stepMs) {
				host.Tick(now);
				client.Tick(now);
				if (done()) {
					return true;
				}
				hostTransport.AdvanceTimeMs(stepMs);
				clientTransport.AdvanceTimeMs(stepMs);
			}
			*error = "condition not reached; host=" + std::string(NetSession::StateName(host.GetState())) +
			         " client=" + NetSession::StateName(client.GetState());
			return false;
		}

		bool StartPair(uint16_t port, NetSession& host, NetSession& client, LoopbackTransport& hostTransport, LoopbackTransport& clientTransport, NetSessionConfig hostConfig, NetSessionConfig clientConfig, std::string* error) {
			if (!host.StartHost(hostTransport, std::move(hostConfig), error)) {
				return false;
			}
			if (!client.StartClient(clientTransport, "loopback", std::move(clientConfig), error)) {
				return false;
			}
			(void)port;
			return true;
		}

		bool TestHappyPath(std::string* error) {
			const uint16_t port = 42001;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetSession host;
			NetSession client;
			if (!StartPair(port, host, client, hostTransport, clientTransport, MakeConfig(port, 101, "Host"), MakeConfig(port, 202, "Player"), error)) {
				return false;
			}
			if (!DrivePair(hostTransport, clientTransport, host, client, [&] { return host.IsReady() && client.IsReady(); }, error)) {
				return false;
			}
			if (host.GetSessionId() == 0 || client.GetSessionId() != host.GetSessionId()) {
				*error = "session id was not assigned consistently";
				return false;
			}
			if (client.GetLocalPeerId() != 1) {
				*error = "client peer id was not stable";
				return false;
			}
			const std::string report = host.BuildReportJson();
			if (report.find("\"accepted\": true") == std::string::npos || report.find("\"final_state\": \"Ready\"") == std::string::npos) {
				*error = "ready report did not contain accepted Ready state";
				return false;
			}
			if (report.find("\"local_identity\"") == std::string::npos ||
			    report.find("\"deterministic_config_hash\"") == std::string::npos ||
			    report.find("\"num_lua_states\"") == std::string::npos) {
				*error = "ready report did not contain local identity diagnostics";
				return false;
			}
			return true;
		}

		bool TestAssignedPeerIdIgnoresTransportPeerId(std::string* error) {
			const uint16_t port = 42002;
			ScriptedHostTransport transport;
			NetSession host;
			NetSessionConfig hostConfig = MakeConfig(port, 1301, "Host");
			hostConfig.maxPeers = 1;
			if (!host.StartHost(transport, hostConfig, error)) {
				return false;
			}

			NetMessage helloMessage;
			helloMessage.sequence = 1;
			helloMessage.payload = MakeClientHello(MakeConfig(port, 1401, "Player"));
			std::vector<uint8_t> helloBytes;
			NetProtocolError encodeError;
			if (!NetProtocol::Encode(helloMessage, helloBytes, &encodeError)) {
				*error = "could not encode scripted ClientHello: " + encodeError.message;
				return false;
			}

			transport.Push({NetTransportEventType::PeerConnected, 300, NetTransportLane::ControlReliable, {}, ""});
			transport.Push({NetTransportEventType::PacketReceived, 300, NetTransportLane::ControlReliable, helloBytes, ""});
			host.Tick(0);

			if (host.GetState() != NetSessionState::Accepted || transport.sentPackets.size() != 2) {
				*error = "scripted host did not accept and send both accept messages";
				return false;
			}

			bool sawHostHello = false;
			bool sawJoinAccepted = false;
			for (const ScriptedHostTransport::SentPacket& packet : transport.sentPackets) {
				if (packet.peerId != 300) {
					*error = "host sent response to the wrong transport peer";
					return false;
				}
				const NetDecodeResult decoded = NetProtocol::Decode(packet.bytes);
				if (!decoded.ok) {
					*error = "host sent undecodable scripted response";
					return false;
				}
				if (const auto* hostHello = std::get_if<NetHostHello>(&decoded.message.payload)) {
					sawHostHello = true;
					if (hostHello->assignedPeerId != 1) {
						*error = "HostHello leaked the transport peer id into the session peer id";
						return false;
					}
				} else if (const auto* accepted = std::get_if<NetJoinAccepted>(&decoded.message.payload)) {
					sawJoinAccepted = true;
					if (accepted->assignedPeerId != 1) {
						*error = "JoinAccepted leaked the transport peer id into the session peer id";
						return false;
					}
				}
			}
			if (!sawHostHello || !sawJoinAccepted) {
				*error = "scripted host did not send HostHello and JoinAccepted";
				return false;
			}
			return true;
		}

		// Nothing bounded the host's tracked connections before a hello arrived, so a silent joiner could
		// grow the list until it timed out. The bound must refuse the excess without disturbing anyone
		// who is already playing.
		bool TestUnauthenticatedConnectionBound(std::string* error) {
			const uint16_t port = 42011;
			ScriptedHostTransport transport;
			NetSession host;
			NetSessionConfig hostConfig = MakeConfig(port, 1601, "Host");
			hostConfig.maxPeers = 2;
			hostConfig.timeoutMs = 5000;
			if (!host.StartHost(transport, hostConfig, error)) {
				return false;
			}

			const auto encode = [error](NetPayload payload, std::vector<uint8_t>& bytes) {
				NetMessage message;
				message.sequence = 1;
				message.payload = std::move(payload);
				NetProtocolError encodeError;
				if (!NetProtocol::Encode(message, bytes, &encodeError)) {
					*error = "could not encode scripted message: " + encodeError.message;
					return false;
				}
				return true;
			};

			// Two players are in and ready before the flood starts.
			for (uint8_t slot = 0; slot < 2; ++slot) {
				const NetPeerId transportPeerId = 100U + slot;
				std::vector<uint8_t> helloBytes;
				if (!encode(MakeClientHello(MakeConfig(port, 1700 + slot, "Player" + std::to_string(slot))), helloBytes)) {
					return false;
				}
				transport.Push({NetTransportEventType::PeerConnected, transportPeerId, NetTransportLane::ControlReliable, {}, ""});
				transport.Push({NetTransportEventType::PacketReceived, transportPeerId, NetTransportLane::ControlReliable, helloBytes, ""});
				host.Tick(0);
				std::vector<uint8_t> readyBytes;
				if (!encode(NetReadyState{static_cast<uint8_t>(slot + 1), true, hostConfig.localIdentity.deterministicConfigHash, hostConfig.localIdentity.moduleManifestHash}, readyBytes)) {
					return false;
				}
				transport.Push({NetTransportEventType::PacketReceived, transportPeerId, NetTransportLane::ControlReliable, readyBytes, ""});
				host.Tick(0);
			}
			if (host.GetReadyPeerCount() != 2) {
				*error = "the two players did not reach ready before the flood";
				return false;
			}

			const std::vector<NetSessionPeerInfo> before = host.GetReadyPeers();
			const auto sameSeats = [](const std::vector<NetSessionPeerInfo>& lhs, const std::vector<NetSessionPeerInfo>& rhs) {
				return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](const NetSessionPeerInfo& a, const NetSessionPeerInfo& b) {
					return a.transportPeerId == b.transportPeerId && a.assignedPeerId == b.assignedPeerId && a.displayName == b.displayName && a.ready == b.ready;
				});
			};

			// Eight silent connections fill the bound; the ninth is refused the way a full session is.
			for (uint32_t i = 0; i < NetSession::c_MaxUnauthenticatedPeers; ++i) {
				transport.Push({NetTransportEventType::PeerConnected, 200U + i, NetTransportLane::ControlReliable, {}, ""});
			}
			host.Tick(10);
			if (host.GetUnauthenticatedPeerCount() != NetSession::c_MaxUnauthenticatedPeers) {
				*error = "the host did not track the allowed half-open connections: " + std::to_string(host.GetUnauthenticatedPeerCount());
				return false;
			}
			if (host.GetStats().unauthenticatedConnectionsRefused != 0) {
				*error = "a connection inside the bound was refused";
				return false;
			}

			const size_t packetsBefore = transport.sentPackets.size();
			transport.Push({NetTransportEventType::PeerConnected, 300, NetTransportLane::ControlReliable, {}, ""});
			host.Tick(20);
			if (host.GetUnauthenticatedPeerCount() != NetSession::c_MaxUnauthenticatedPeers) {
				*error = "the ninth half-open connection was tracked past the bound";
				return false;
			}
			if (host.GetStats().unauthenticatedConnectionsRefused != 1) {
				*error = "the refused connection was not counted";
				return false;
			}
			if (transport.sentPackets.size() != packetsBefore + 1) {
				*error = "the refused connection did not get exactly one reply";
				return false;
			}
			const NetDecodeResult refusal = NetProtocol::Decode(transport.sentPackets.back().bytes);
			const auto* rejected = refusal.ok ? std::get_if<NetJoinRejected>(&refusal.message.payload) : nullptr;
			if (transport.sentPackets.back().peerId != 300 || rejected == nullptr || rejected->rejectReason != NetRejectReason::SessionFull) {
				*error = "the refused connection was not rejected the way a full session rejects";
				return false;
			}

			// The players already in the match are untouched by the flood and the refusal.
			if (host.GetReadyPeerCount() != 2 || !sameSeats(host.GetReadyPeers(), before)) {
				*error = "the flood disturbed the players already in the session";
				return false;
			}
			if (host.IsFailed() || host.IsClosed() || host.GetState() != NetSessionState::Ready) {
				*error = "the host left the ready state under the flood";
				return false;
			}

			// A silent connection still ages out at the session timeout, and its slot comes back.
			std::vector<uint8_t> heartbeatBytes;
			if (!encode(NetHeartbeat{5900, 0, 0}, heartbeatBytes)) {
				return false;
			}
			for (uint8_t slot = 0; slot < 2; ++slot) {
				transport.Push({NetTransportEventType::PacketReceived, 100U + slot, NetTransportLane::ControlReliable, heartbeatBytes, ""});
			}
			host.Tick(5900);
			if (host.GetUnauthenticatedPeerCount() != 0) {
				*error = "half-open connections did not time out";
				return false;
			}
			transport.Push({NetTransportEventType::PeerConnected, 400, NetTransportLane::ControlReliable, {}, ""});
			host.Tick(5910);
			if (host.GetUnauthenticatedPeerCount() != 1 || host.GetStats().unauthenticatedConnectionsRefused != 1) {
				*error = "a fresh connection was refused after the bound cleared";
				return false;
			}
			if (host.GetReadyPeerCount() != 2 || !sameSeats(host.GetReadyPeers(), before)) {
				*error = "the players did not survive the whole sequence";
				return false;
			}
			return true;
		}

		bool TestReadyRequiresAcceptedConnection(std::string* error) {
			ScriptedHostTransport transport;
			NetSession host;
			const NetSessionConfig config = MakeConfig(42207, 1501, "Host");
			if (!host.StartHost(transport, config, error)) return false;
			NetMessage ready;
			ready.payload = NetReadyState{0, true, config.localIdentity.deterministicConfigHash, config.localIdentity.moduleManifestHash};
			std::vector<uint8_t> bytes;
			if (!NetProtocol::Encode(ready, bytes)) return false;
			transport.Push({NetTransportEventType::PeerConnected, 300, NetTransportLane::ControlReliable, {}, ""});
			transport.Push({NetTransportEventType::PacketReceived, 300, NetTransportLane::ControlReliable, bytes, ""});
			host.Tick(0);
			if (host.GetState() != NetSessionState::Listening || host.GetReadyPeerCount() != 0 ||
			    !host.HasReject() || host.GetRejectReason() != NetRejectReason::HostNotAccepting) {
				*error = "Ready bypassed the connection handshake";
				return false;
			}
			transport.Push({NetTransportEventType::PacketReceived, 300, NetTransportLane::ControlReliable, bytes, ""});
			host.Tick(10);
			if (host.GetState() != NetSessionState::Listening || host.GetReadyPeerCount() != 0) {
				*error = "late Ready revived a rejected connection";
				return false;
			}
			transport.Push({NetTransportEventType::LocalTransportFault, c_InvalidNetPeerId, NetTransportLane::ControlReliable, {}, "pump failed"});
			transport.Push({NetTransportEventType::PeerConnected, 301, NetTransportLane::ControlReliable, {}, ""});
			host.Tick(20);
			if (!host.IsFailed()) {
				*error = "a connection event cleared the local transport failure";
				return false;
			}
			return true;
		}

		bool TestRejectCase(const std::string& name, const std::function<void(NetSessionConfig&)>& mutateClient, NetRejectReason expectedReason, const std::string& expectedKey, std::string* error) {
			static uint16_t nextPort = 42100;
			const uint16_t port = nextPort++;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetSession host;
			NetSession client;
			NetSessionConfig hostConfig = MakeConfig(port, 301, "Host");
			NetSessionConfig clientConfig = MakeConfig(port, 401, "Player");
			mutateClient(clientConfig);
			if (!StartPair(port, host, client, hostTransport, clientTransport, std::move(hostConfig), std::move(clientConfig), error)) {
				return false;
			}
			if (!DrivePair(hostTransport, clientTransport, host, client, [&] { return client.IsRejected(); }, error)) {
				*error = name + ": " + *error;
				return false;
			}
			if (client.GetRejectReason() != expectedReason || client.GetMismatchKey() != expectedKey) {
				*error = name + ": expected " + NetProtocol::RejectReasonName(expectedReason) + "/" + expectedKey +
				         " got " + NetProtocol::RejectReasonName(client.GetRejectReason()) + "/" + client.GetMismatchKey();
				return false;
			}
			return true;
		}

		bool TestRejects(std::string* error) {
			if (!TestRejectCase("protocol mismatch", [](NetSessionConfig& c) {
					c.minProtocolVersion = NetProtocol::c_Version + 1;
					c.maxProtocolVersion = NetProtocol::c_Version + 1;
				}, NetRejectReason::ProtocolMismatch, "network_protocol_version", error)) return false;
			if (!TestRejectCase("game version mismatch", [](NetSessionConfig& c) {
					c.localIdentity.gameVersion = "7.0.1-test";
				}, NetRejectReason::GameVersionMismatch, "game_version", error)) return false;
			if (!TestRejectCase("build mismatch", [](NetSessionConfig& c) {
					c.localIdentity.buildId = "other-build";
				}, NetRejectReason::BuildMismatch, "build_id", error)) return false;
			if (!TestRejectCase("controller version mismatch", [](NetSessionConfig& c) {
					++c.localIdentity.controllerFrameVersion;
				}, NetRejectReason::ControllerFrameVersionMismatch, "controller_frame_version", error)) return false;
			if (!TestRejectCase("controller size mismatch", [](NetSessionConfig& c) {
					c.localIdentity.controllerFrameEncodedSize += 4;
				}, NetRejectReason::ControllerFrameSizeMismatch, "controller_frame_encoded_size", error)) return false;
			if (!TestRejectCase("config mismatch", [](NetSessionConfig& c) {
					c.localIdentity.deterministicConfigHash = MakeHash(2);
				}, NetRejectReason::DeterministicConfigMismatch, "deterministic_config_hash", error)) return false;
			if (!TestRejectCase("module mismatch", [](NetSessionConfig& c) {
					c.localIdentity.moduleManifestHash = MakeHash(34);
				}, NetRejectReason::ModuleManifestMismatch, "module_manifest_hash", error)) return false;
			if (!TestRejectCase("session rules mismatch", [](NetSessionConfig& c) {
					c.localIdentity.sessionRulesHash = MakeHash(66);
				}, NetRejectReason::SessionRulesMismatch, "session_rules_hash", error)) return false;
			if (!TestRejectCase("userdata rejection", [](NetSessionConfig& c) {
					c.localIdentity.hasUserdataModules = true;
				}, NetRejectReason::UserdataModulesNotAllowed, "userdata_modules", error)) return false;
			return true;
		}

		bool TestSessionFull(std::string* error) {
			const uint16_t port = 42201;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransportA;
			LoopbackTransport clientTransportB;
			NetSession host;
			NetSession clientA;
			NetSession clientB;
			NetSessionConfig hostConfig = MakeConfig(port, 501, "Host");
			hostConfig.maxPeers = 1;
			if (!host.StartHost(hostTransport, hostConfig, error) ||
			    !clientA.StartClient(clientTransportA, "loopback", MakeConfig(port, 601, "A"), error)) {
				return false;
			}
			if (!DrivePair(hostTransport, clientTransportA, host, clientA, [&] { return host.IsReady() && clientA.IsReady(); }, error)) {
				return false;
			}
			if (!clientB.StartClient(clientTransportB, "loopback", MakeConfig(port, 602, "B"), error)) {
				return false;
			}
			for (uint64_t now = 0; now <= 1000; now += 10) {
				host.Tick(now);
				clientA.Tick(now);
				clientB.Tick(now);
				if (clientB.IsRejected()) {
					break;
				}
				hostTransport.AdvanceTimeMs(10);
				clientTransportA.AdvanceTimeMs(10);
				clientTransportB.AdvanceTimeMs(10);
			}
			if (!clientB.IsRejected() || clientB.GetRejectReason() != NetRejectReason::SessionFull) {
				*error = "session full did not reject second client";
				return false;
			}
			if (!host.IsReady() || !clientA.IsReady()) {
				*error = "session full rejection stopped the accepted peer";
				return false;
			}
			return true;
		}

		bool TestDuplicateNonce(std::string* error) {
			const uint16_t port = 42202;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransportA;
			LoopbackTransport clientTransportB;
			NetSession host;
			NetSession clientA;
			NetSession clientB;
			NetSessionConfig hostConfig = MakeConfig(port, 701, "Host");
			hostConfig.maxPeers = 2;
			if (!host.StartHost(hostTransport, hostConfig, error) ||
			    !clientA.StartClient(clientTransportA, "loopback", MakeConfig(port, 801, "A"), error)) {
				return false;
			}
			if (!DrivePair(hostTransport, clientTransportA, host, clientA, [&] { return host.IsReady() && clientA.IsReady(); }, error)) {
				return false;
			}
			if (!clientB.StartClient(clientTransportB, "loopback", MakeConfig(port, 801, "B"), error)) {
				return false;
			}
			for (uint64_t now = 0; now <= 1000; now += 10) {
				host.Tick(now);
				clientA.Tick(now);
				clientB.Tick(now);
				if (clientB.IsRejected()) {
					break;
				}
				hostTransport.AdvanceTimeMs(10);
				clientTransportA.AdvanceTimeMs(10);
				clientTransportB.AdvanceTimeMs(10);
			}
			if (!clientB.IsRejected() || clientB.GetRejectReason() != NetRejectReason::DuplicateClientNonce) {
				*error = "duplicate client nonce was not rejected";
				return false;
			}
			if (!host.IsReady() || !clientA.IsReady()) {
				*error = "duplicate nonce rejection stopped the accepted peer";
				return false;
			}
			return true;
		}

		bool TestPeerTimeoutDoesNotStopHost(std::string* error) {
			const uint16_t port = 42203;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransportA;
			LoopbackTransport clientTransportB;
			NetSession host;
			NetSession clientA;
			NetSession clientB;
			NetSessionConfig hostConfig = MakeConfig(port, 1301, "Host");
			hostConfig.maxPeers = 2;
			hostConfig.heartbeatIntervalMs = 10;
			hostConfig.timeoutMs = 50;
			if (!host.StartHost(hostTransport, hostConfig, error) ||
			    !clientA.StartClient(clientTransportA, "loopback", MakeConfig(port, 1401, "A"), error)) {
				return false;
			}

			bool startedB = false;
			bool bothReady = false;
			uint64_t readyAtMs = 0;
			for (uint64_t now = 0; now <= 1000; now += 10) {
				host.Tick(now);
				clientA.Tick(now);
				if (startedB) {
					clientB.Tick(now);
				}
				if (!startedB && host.IsReady() && clientA.IsReady()) {
					if (!clientB.StartClient(clientTransportB, "loopback", MakeConfig(port, 1402, "B"), error)) {
						return false;
					}
					startedB = true;
				}
				if (startedB && host.IsReady() && clientA.IsReady() && clientB.IsReady()) {
					bothReady = true;
					readyAtMs = now;
					break;
				}
				hostTransport.AdvanceTimeMs(10);
				clientTransportA.AdvanceTimeMs(10);
				clientTransportB.AdvanceTimeMs(10);
			}
			if (!bothReady) {
				*error = "two-peer ready state was not reached";
				return false;
			}

			for (uint64_t now = readyAtMs + 10; now <= readyAtMs + 200; now += 10) {
				host.Tick(now);
				clientA.Tick(now);
				if (host.IsRejected() || host.IsFailed()) {
					*error = "one peer timeout stopped the host session";
					return false;
				}
				if (host.GetStats().timeouts > 0) {
					break;
				}
				hostTransport.AdvanceTimeMs(10);
				clientTransportA.AdvanceTimeMs(10);
				clientTransportB.AdvanceTimeMs(10);
			}
			if (host.GetStats().timeouts == 0) {
				*error = "second peer did not time out";
				return false;
			}
			if (!host.IsReady() || !clientA.IsReady()) {
				*error = "one peer timeout did not preserve the ready peer";
				return false;
			}
			return true;
		}

		bool TestMalformedHandshake(std::string* error) {
			const uint16_t port = 42204;
			LoopbackTransport hostTransport;
			LoopbackTransport rawClient;
			NetSession host;
			if (!host.StartHost(hostTransport, MakeConfig(port, 901, "Host"), error) ||
			    !rawClient.Connect("loopback", port, error)) {
				return false;
			}
			host.Tick(0);
			rawClient.PollEvents();
			if (!rawClient.Send(1, NetTransportLane::ControlReliable, {1, 2, 3}, error)) {
				return false;
			}
			host.Tick(10);
			if (host.GetState() != NetSessionState::Listening || !host.HasReject() ||
			    host.GetRejectReason() != NetRejectReason::MalformedMessage || host.GetStats().malformedMessages != 1) {
				*error = "malformed handshake did not produce MalformedMessage";
				return false;
			}
			LoopbackTransport replacementTransport;
			NetSession replacement;
			if (!replacement.StartClient(replacementTransport, "loopback", MakeConfig(port, 902, "Replacement"), error)) return false;
			return DrivePair(hostTransport, replacementTransport, host, replacement, [&] {
				return host.IsReady() && replacement.IsReady() && host.GetReadyPeerCount() == 1;
			}, error);
		}

		bool TestTimeout(std::string* error) {
			const uint16_t port = 42205;
			LoopbackTransport hostTransport;
			LoopbackTransport rawClient;
			NetSession host;
			NetSessionConfig hostConfig = MakeConfig(port, 1001, "Host");
			hostConfig.timeoutMs = 50;
			if (!host.StartHost(hostTransport, hostConfig, error) ||
			    !rawClient.Connect("loopback", port, error)) {
				return false;
			}
			for (uint64_t now = 0; now <= 200; now += 10) {
				host.Tick(now);
				if (host.GetStats().timeouts > 0) {
					break;
				}
				hostTransport.AdvanceTimeMs(10);
				rawClient.AdvanceTimeMs(10);
			}
			if (host.GetState() != NetSessionState::Listening || !host.HasReject() ||
			    host.GetRejectReason() != NetRejectReason::Timeout || host.GetStats().timeouts != 1) {
				*error = "timeout path did not record Timeout";
				return false;
			}
			LoopbackTransport replacementTransport;
			NetSession replacement;
			if (!replacement.StartClient(replacementTransport, "loopback", MakeConfig(port, 1002, "Replacement"), error)) return false;
			return DrivePair(hostTransport, replacementTransport, host, replacement, [&] {
				return host.IsReady() && replacement.IsReady() && host.GetReadyPeerCount() == 1;
			}, error);
		}

		bool TestLobbyMembership(std::string* error) {
			constexpr uint16_t port = 42208;
			LoopbackTransport hostTransport;
			NetSession hostSession;
			NetSessionConfig hostConfig = MakeConfig(port, 1601, "Host");
			hostConfig.maxPeers = 2;
			hostConfig.timeoutMs = 1000;
			if (!hostSession.StartHost(hostTransport, hostConfig, error)) return false;
			NetMatchConfig match = NetMatchConfigUtil::MakeDefault(hostConfig.sessionId);
			match.peerCount = 3;
			match.players.push_back({3, 2, false, "Client B"});
			NetLobbySessionConfig config;
			config.host = true;
			config.localPeerId = 1;
			config.matchConfig = match;
			config.autoStart = false;
			config.session = &hostSession;
			config.peerStateIntervalMs = 10;
			NetLobbySession hostLobby;
			if (!hostLobby.Start(hostTransport, config, error)) return false;
			std::array<LoopbackTransport, 2> transports;
			std::array<NetSession, 2> sessions;
			std::array<NetLobbySession, 2> lobbies;
			std::array<bool, 2> active{}, started{}, autoReady{};
			std::array<uint64_t, 2> startedAt{};
			std::array<std::string, 2> names;
			uint64_t now = 0;
			std::string phase = "initial join";
			auto join = [&](size_t i, const char* name, bool ready) {
				names[i] = name;
				active[i] = true;
				started[i] = false;
				autoReady[i] = ready;
				return sessions[i].StartClient(transports[i], "loopback", MakeConfig(port, 1701 + i, name), error);
			};
			auto pump = [&]() {
				hostLobby.Tick(now);
				for (size_t i = 0; i < active.size(); ++i) {
					if (!active[i]) continue;
					if (!started[i]) {
						sessions[i].Tick(now);
						if (sessions[i].IsReady()) {
							NetLobbySessionConfig client = config;
							client.host = false;
							client.localPeerId = static_cast<uint8_t>(sessions[i].GetLocalPeerId() + 1);
							client.remoteTransportPeerIds = {{1, sessions[i].GetRemoteTransportPeerId()}};
							client.session = &sessions[i];
							client.displayName = names[i];
							client.autoReady = autoReady[i];
							if (!lobbies[i].Start(transports[i], client, error)) return false;
							started[i] = true;
							startedAt[i] = now;
						}
					} else {
						lobbies[i].Tick(now - startedAt[i]);
					}
					if (lobbies[i].IsFailed() || lobbies[i].IsRejected()) {
						*error = "membership " + phase + " client " + names[i] + " at " + std::to_string(now) + ": " + lobbies[i].GetFailureReason() + "; " + hostSession.BuildReportJson();
						return false;
					}
				}
				if (hostLobby.IsFailed() || hostLobby.IsRejected()) {
					*error = "membership host failed: " + hostLobby.GetFailureReason();
					return false;
				}
				hostTransport.AdvanceTimeMs(10);
				for (auto& transport: transports) transport.AdvanceTimeMs(10);
				now += 10;
				return true;
			};
			auto until = [&](const std::function<bool()>& done) {
				for (int i = 0; i < 300; ++i) {
					if (!pump()) return false;
					if (done()) return true;
				}
				*error = "lobby membership condition timed out";
				return false;
			};
			if (!join(0, "Client A", true) || !join(1, "Client B", true) ||
			    !until([&] { return hostLobby.IsRemoteReady() && lobbies[0].HasHeardFrom(3) && lobbies[1].HasHeardFrom(2); })) return false;
			LoopbackTransport unbound;
			phase = "unbound admission";
			if (!unbound.Connect("loopback", port, error)) return false;
			std::vector<uint8_t> bytes;
			if (!NetLobbyProtocol::Encode({NetLobbyReady{2, false}}, bytes) || !unbound.Send(1, NetTransportLane::ControlReliable, bytes, error) || !pump()) return false;
			if (!hostLobby.IsRemoteReady() || hostSession.GetReadyPeerCount() != 2) {
				*error = "unaccepted connection changed lobby readiness";
				return false;
			}
			if (!unbound.Send(1, NetTransportLane::ControlReliable, {1, 2, 3}, error) || !pump()) return false;
			std::vector<uint8_t> state(5 * NetLobbyProtocol::c_MaxStateChunkBytes + 17);
			for (size_t i = 0; i < state.size(); ++i) state[i] = static_cast<uint8_t>(i * 17);
			hostLobby.BeginStateTransfer(state);
			phase = "partial state transfer";
			if (!pump()) return false;
			sessions[0].Close("left lobby");
			phase = "client leave";
			active[0] = false;
			if (!until([&] { return hostSession.GetReadyPeerCount() == 1 && !lobbies[1].HasHeardFrom(2); })) return false;
			if (hostLobby.IsRemoteReady() || !hostLobby.IsRemoteReady(3) || !lobbies[1].IsLocalReady()) {
				*error = "leave changed the remaining player's readiness or left the room startable";
				return false;
			}
			hostLobby.RequestStart();
			phase = "replacement";
			if (!join(0, "Returner", false) || !until([&] { return hostLobby.GetState() == NetLobbyState::WaitingForReady && started[0]; })) return false;
			if (hostLobby.IsStarted() || lobbies[0].IsLocalReady() || hostLobby.GetRemoteName(2) != "Returner") {
				*error = "replacement inherited readiness or stale identity";
				return false;
			}
			lobbies[0].SetLocalReady(true);
			phase = "replacement state transfer";
			if (!until([&] { return hostLobby.IsRemoteReady() && lobbies[0].HasCompleteStateTransfer() && lobbies[1].HasCompleteStateTransfer(); })) return false;
			if (hostLobby.IsStarted() || hostLobby.IsStartRequested() || lobbies[0].TakeReceivedState() != state || lobbies[1].TakeReceivedState() != state) {
				*error = "replacement lost state-transfer bytes or retained an obsolete Start request";
				return false;
			}
			if (!NetLobbyProtocol::Encode({NetLobbyReady{2, false}}, bytes) || !transports[1].Send(1, NetTransportLane::ControlReliable, bytes, error)) return false;
			active[1] = false;
			phase = "forged readiness";
			if (!pump()) return false;
			sessions[1].Tick(now);
			if (!sessions[1].IsClosed() || hostSession.GetReadyPeerCount() != 1 || !hostLobby.IsRemoteReady(2)) {
				*error = "forged readiness was not isolated to its sending connection";
				return false;
			}
			phase = "replacement after rejection";
			if (!join(1, "Newcomer", true) || !until([&] { return hostLobby.IsRemoteReady() && hostLobby.GetState() == NetLobbyState::WaitingForReady; })) return false;
			hostLobby.RequestStart();
			if (!until([&] { return hostLobby.IsStarted() && lobbies[0].IsStarted() && lobbies[1].IsStarted(); })) return false;
			if (hostLobby.GetMatchConfigHash() != lobbies[0].GetMatchConfigHash() || hostLobby.GetMatchConfigHash() != lobbies[1].GetMatchConfigHash()) {
				*error = "replacement lobby started with different configs";
				return false;
			}
			return true;
		}

		// A match needs a few KB/s and only the state stream needs megabytes, so the rate is raised for
		// the transfer and dropped after it - after it has left the socket, not after the last chunk was
		// handed to it, or the tail of the stream drains at the match rate with the round behind it.
		bool TestBulkRateFollowsTheSocket(std::string* error) {
			constexpr uint16_t port = 42209;
			constexpr uint64_t latencyMs = 40;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			LoopbackTransportConfig faults;
			faults.latencyMs = latencyMs;
			hostTransport.SetFaultConfig(faults);
			clientTransport.SetFaultConfig(faults);
			NetSession hostSession;
			NetSessionConfig hostConfig = MakeConfig(port, 1801, "Host");
			hostConfig.maxPeers = 1;
			if (!hostSession.StartHost(hostTransport, hostConfig, error)) {
				return false;
			}
			NetSession clientSession;
			if (!clientSession.StartClient(clientTransport, "loopback", MakeConfig(port, 1802, "Player"), error)) {
				return false;
			}
			NetMatchConfig match = NetMatchConfigUtil::MakeDefault(hostConfig.sessionId);
			match.peerCount = 2;
			NetLobbySessionConfig config;
			config.host = true;
			config.localPeerId = 1;
			config.matchConfig = match;
			config.autoStart = false;
			config.session = &hostSession;
			NetLobbySession hostLobby;
			if (!hostLobby.Start(hostTransport, config, error)) {
				return false;
			}
			uint64_t now = 0;
			auto pump = [&]() {
				hostSession.Tick(now);
				clientSession.Tick(now);
				hostLobby.Tick(now);
				hostTransport.AdvanceTimeMs(10);
				clientTransport.AdvanceTimeMs(10);
				now += 10;
			};
			for (int i = 0; i < 200 && !(hostSession.IsReady() && clientSession.IsReady()); ++i) {
				pump();
			}
			if (!hostSession.IsReady() || !clientSession.IsReady()) {
				*error = "the bulk-rate fixture never got a peer onto the lobby";
				return false;
			}
			if (hostTransport.IsBulkTransferMode()) {
				*error = "the transport was already carrying the bulk rate before any transfer";
				return false;
			}
			std::vector<uint8_t> state(5 * NetLobbyProtocol::c_MaxStateChunkBytes + 17);
			for (size_t i = 0; i < state.size(); ++i) {
				state[i] = static_cast<uint8_t>(i * 17);
			}
			hostLobby.BeginStateTransfer(state);
			if (!hostTransport.IsBulkTransferMode()) {
				*error = "the stream was queued at the match rate";
				return false;
			}
			// Every chunk handed over, and the link still carrying them: the rate must not drop yet.
			bool sawQueueEmptyWithBytesInFlight = false;
			for (int i = 0; i < 400 && hostTransport.IsBulkTransferMode(); ++i) {
				pump();
				if (!hostLobby.HasPendingStateChunks() && hostTransport.InFlightBytes() > 0) {
					sawQueueEmptyWithBytesInFlight = true;
					if (!hostTransport.IsBulkTransferMode()) {
						*error = "the rate dropped while the stream was still on the link";
						return false;
					}
				}
			}
			if (!sawQueueEmptyWithBytesInFlight) {
				*error = "the fixture never observed the stream in flight past the last queued chunk";
				return false;
			}
			if (hostTransport.IsBulkTransferMode()) {
				*error = "the bulk rate was never given back";
				return false;
			}
			const std::vector<LoopbackRateChange>& changes = hostTransport.GetRateChanges();
			if (changes.size() != 2 || !changes[0].bulk || changes[1].bulk) {
				*error = "expected exactly one raise then one drop, got " + std::to_string(changes.size());
				return false;
			}
			if (changes[1].atMs < changes[0].atMs + latencyMs) {
				*error = "the drop came before the stream could have left the link";
				return false;
			}
			// A torn-down transport keeps no rate latched for the next lobby.
			hostTransport.Stop();
			if (hostTransport.IsBulkTransferMode()) {
				*error = "the transport stayed latched at the bulk rate through Stop()";
				return false;
			}
			std::cout << "[net-session-selftest] PASS bulk_rate_follows_the_socket raised_at=" << changes[0].atMs
			          << "ms dropped_at=" << changes[1].atMs << "ms state_bytes=" << state.size() << std::endl;
			return true;
		}

		bool TestLatencyAndCleanDisconnect(std::string* error) {
			const uint16_t port = 42206;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			LoopbackTransportConfig faults;
			faults.latencyMs = 40;
			faults.reorderUnreliable = true;
			hostTransport.SetFaultConfig(faults);
			clientTransport.SetFaultConfig(faults);
			NetSession host;
			NetSession client;
			if (!StartPair(port, host, client, hostTransport, clientTransport, MakeConfig(port, 1101, "Host"), MakeConfig(port, 1201, "Player"), error)) {
				return false;
			}
			if (!DrivePair(hostTransport, clientTransport, host, client, [&] { return host.IsReady() && client.IsReady(); }, error, 2000)) {
				return false;
			}
			client.Close("done");
			if (!DrivePair(hostTransport, clientTransport, host, client, [&] { return host.GetState() == NetSessionState::Listening && client.IsClosed(); }, error, 1000)) {
				return false;
			}
			const uint64_t sessionId = host.GetSessionId();
			if (!client.StartClient(clientTransport, "loopback", MakeConfig(port, 1201, "Replacement"), error) ||
			    !DrivePair(hostTransport, clientTransport, host, client, [&] { return host.IsReady() && client.IsReady(); }, error, 2000)) {
				return false;
			}
			if (client.GetLocalPeerId() != 1 || client.GetSessionId() != sessionId || host.GetReadyPeerCount() != 1 ||
			    host.GetReadyPeers().front().displayName != "Replacement") {
				*error = "replacement did not reuse the vacant seat in the same session";
				return false;
			}
			host.Close("host ended lobby");
			return DrivePair(hostTransport, clientTransport, host, client, [&] { return host.IsClosed() && client.IsClosed(); }, error, 1000);
		}
	}

	int NetSessionSelfTest::Run() {
		auto fail = [](const std::string& message) {
			std::cerr << "[net-session-selftest] FAIL: " << message << std::endl;
			return 1;
		};

		std::string error;
		if (!TestHappyPath(&error)) return fail(error);
		if (!TestAssignedPeerIdIgnoresTransportPeerId(&error)) return fail(error);
		if (!TestReadyRequiresAcceptedConnection(&error)) return fail(error);
		if (!TestRejects(&error)) return fail(error);
		if (!TestSessionFull(&error)) return fail(error);
		if (!TestUnauthenticatedConnectionBound(&error)) return fail(error);
		if (!TestDuplicateNonce(&error)) return fail(error);
		if (!TestPeerTimeoutDoesNotStopHost(&error)) return fail(error);
		if (!TestMalformedHandshake(&error)) return fail(error);
		if (!TestTimeout(&error)) return fail(error);
		if (!TestLatencyAndCleanDisconnect(&error)) return fail(error);
		if (!TestLobbyMembership(&error)) return fail(error);
		if (!TestBulkRateFollowsTheSocket(&error)) return fail(error);

		std::cout << "[net-session-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
