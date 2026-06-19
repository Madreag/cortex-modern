#include "NetSessionSelfTest.h"

#include "ControllerFrame.h"
#include "LoopbackTransport.h"
#include "NetSession.h"

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
			bool Send(NetPeerId peerId, NetTransportLane, const std::vector<uint8_t>& bytes, std::string*) override {
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
			if (!host.IsRejected() || host.GetRejectReason() != NetRejectReason::MalformedMessage || host.GetStats().malformedMessages != 1) {
				*error = "malformed handshake did not produce MalformedMessage";
				return false;
			}
			return true;
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
				if (host.IsRejected() || host.IsFailed()) {
					break;
				}
				hostTransport.AdvanceTimeMs(10);
				rawClient.AdvanceTimeMs(10);
			}
			if ((!host.IsRejected() && !host.IsFailed()) || host.GetRejectReason() != NetRejectReason::Timeout || host.GetStats().timeouts != 1) {
				*error = "timeout path did not record Timeout";
				return false;
			}
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
			if (!DrivePair(hostTransport, clientTransport, host, client, [&] { return host.IsClosed() && client.IsClosed(); }, error, 1000)) {
				return false;
			}
			return true;
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
		if (!TestRejects(&error)) return fail(error);
		if (!TestSessionFull(&error)) return fail(error);
		if (!TestDuplicateNonce(&error)) return fail(error);
		if (!TestPeerTimeoutDoesNotStopHost(&error)) return fail(error);
		if (!TestMalformedHandshake(&error)) return fail(error);
		if (!TestTimeout(&error)) return fail(error);
		if (!TestLatencyAndCleanDisconnect(&error)) return fail(error);

		std::cout << "[net-session-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
