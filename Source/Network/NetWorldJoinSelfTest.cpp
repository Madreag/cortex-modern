#include "NetWorldJoinSelfTest.h"

#include "ControllerFrame.h"
#include "LoopbackTransport.h"
#include "NetAuthCrypto.h"
#include "NetDirectoryCodec.h"
#include "NetIdentity.h"
#include "NetLockstep.h"
#include "NetMatchConfig.h"
#include "NetReconnectAdmission.h"
#include "NetReconnectSession.h"
#include "NetSeatAuth.h"
#include "NetDirectoryClient.h"
#include "NetLobbySession.h"
#include "NetMatchService.h"
#include "NetReconnectTicketStore.h"
#include "NetWorldJoin.h"
#include "System/ScenarioRunner.h"

#include "nlohmann/json.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <variant>

namespace RTE {

	namespace {
		const char* s_FailTag = "net-world-join-selftest";

		int Fail(const std::string& message) {
			std::cerr << "[" << s_FailTag << "] FAIL: " << message << std::endl;
			return 1;
		}

		int Pass() {
			std::cout << "[" << s_FailTag << "] PASS" << std::endl;
			return 0;
		}

		const char* c_WorldId = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";

		class ScriptedAuthCrypto : public NetAuthCrypto {
		public:
			bool IsRealCrypto() const override { return false; }
			bool RandomBytes(uint8_t* buffer, size_t count) override {
				if (buffer == nullptr) {
					return false;
				}
				for (size_t i = 0; i < count; ++i) {
					m_Counter = static_cast<uint8_t>(m_Counter * 37U + 149U);
					buffer[i] = m_Counter;
				}
				return true;
			}
			bool HmacSha256(const uint8_t* key, size_t keyCount, const uint8_t* message, size_t messageCount, uint8_t (&mac)[32]) override {
				if (key == nullptr || keyCount == 0) {
					return false;
				}
				uint64_t fold = 1469598103934665603ull;
				for (size_t i = 0; i < keyCount; ++i) {
					fold = (fold ^ key[i]) * 1099511628211ull;
				}
				for (size_t i = 0; i < messageCount; ++i) {
					fold = (fold ^ message[i]) * 1099511628211ull;
				}
				for (size_t i = 0; i < sizeof(mac); ++i) {
					mac[i] = static_cast<uint8_t>(fold >> (8 * (i % 8)));
				}
				return true;
			}

		private:
			uint8_t m_Counter = 1;
		};

		struct ScopedTestCrypto {
			explicit ScopedTestCrypto(NetAuthCrypto* provider) { SetNetAuthCryptoForTest(provider); }
			~ScopedTestCrypto() { SetNetAuthCryptoForTest(nullptr); }
		};

		NetMatchConfig MakeWorldConfig() {
			NetMatchConfig config = NetMatchConfigUtil::MakeDefault(0x574F524C44ULL);
			config.version = NetMatchConfigUtil::c_PersistentWorldVersion;
			config.persistentWorld = true;
			config.dedicated = true;
			config.worldId = c_WorldId;
			config.worldBoot = 1;
			config.peerCount = 2;
			config.hostPeerId = 1;
			config.players = {
				NetMatchPlayerSlot{1, 0, true, "World"},
				NetMatchPlayerSlot{2, 1, false, "Player"},
			};
			return config;
		}

		NetWorldIdentity MakeIdentity(uint64_t boot = 1, uint64_t round = 1) {
			NetWorldIdentity identity;
			identity.worldId = c_WorldId;
			identity.boot = boot;
			identity.round = round;
			return identity;
		}

		NetLockstepFrame MakeCommittedFrame(uint64_t target, uint8_t sender = 1) {
			NetLockstepFrame frame;
			frame.senderPeerId = sender;
			frame.targetFrame = target;
			frame.roundId = 1;
			ControllerFrame controller;
			controller.actorUniqueID = 100 + static_cast<int64_t>(target);
			controller.inputMode = static_cast<uint8_t>(Controller::CIM_PLAYER);
			frame.frames.push_back(controller);
			return frame;
		}

		NetH4Identity MakeH4Identity() {
			NetH4Identity identity;
			identity.controllerFrameVersion = ControllerFrame::c_Version;
			identity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
			identity.gameVersion = "7.0.0-test";
			identity.buildId = "stage2-world";
			identity.deterministicConfigHash.fill(1);
			identity.moduleManifestHash.fill(2);
			identity.sessionRulesHash.fill(3);
			identity.sessionIdentityHash.fill(4);
			return identity;
		}

		/// A host and a client lobby over one in-memory loopback link, the pair the world join plane
		/// streams its image, tail and reports across.
		struct WorldLobbyPair {
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetLobbySession host;
			NetLobbySession client;
			NetPeerId hostRemote = c_InvalidNetPeerId;
			NetPeerId clientRemote = c_InvalidNetPeerId;
			uint64_t nowMs = 0;

			bool Open(uint16_t port, std::string* error) {
				if (!hostTransport.StartHost(port, error) || !clientTransport.Connect("loopback", port, error)) {
					return false;
				}
				for (const NetTransportEvent& event: hostTransport.PollEvents()) {
					if (event.type == NetTransportEventType::PeerConnected) {
						hostRemote = event.peerId;
					}
				}
				for (const NetTransportEvent& event: clientTransport.PollEvents()) {
					if (event.type == NetTransportEventType::PeerConnected) {
						clientRemote = event.peerId;
					}
				}
				if (hostRemote == c_InvalidNetPeerId || clientRemote == c_InvalidNetPeerId) {
					if (error) *error = "loopback peer connection events were not observed";
					return false;
				}
				NetLobbySessionConfig hostConfig;
				hostConfig.host = true;
				hostConfig.localPeerId = 1;
				hostConfig.remotePeerId = 2;
				hostConfig.remoteTransportPeerId = hostRemote;
				hostConfig.matchConfig = MakeWorldConfig();
				hostConfig.autoStart = false;
				NetLobbySessionConfig clientConfig;
				clientConfig.localPeerId = 2;
				clientConfig.remotePeerId = 1;
				clientConfig.remoteTransportPeerId = clientRemote;
				clientConfig.matchConfig = MakeWorldConfig();
				if (!host.Start(hostTransport, hostConfig, error) || !client.Start(clientTransport, clientConfig, error)) {
					return false;
				}
				Pump(8);
				return true;
			}

			void Pump(int rounds) {
				for (int i = 0; i < rounds; ++i) {
					host.PumpOutgoingChunks();
					host.Tick(nowMs);
					client.Tick(nowMs);
					hostTransport.AdvanceTimeMs(10);
					clientTransport.AdvanceTimeMs(10);
					nowMs += 10;
				}
			}
		};

		std::filesystem::path LaneDirectory() {
			// Two launches of the restart case each get their own private runtime, so the driver names
			// the one directory both of them boot their identity from.
			if (const char* shared = std::getenv("CCCP_WORLD_IDENTITY_DIR"); shared != nullptr && shared[0] != '\0') {
				return std::filesystem::path(shared);
			}
			return std::filesystem::current_path() / "Userdata" / "world-join-selftest";
		}

		bool ResetLane(std::string* error) {
			std::error_code code;
			std::filesystem::remove_all(LaneDirectory(), code);
			std::filesystem::create_directories(LaneDirectory(), code);
			if (code) {
				*error = "could not prepare the world identity directory: " + code.message();
				return false;
			}
			return true;
		}

		int TestIdentityPrintOnce() {
			std::string error;
			std::error_code code;
			std::filesystem::create_directories(LaneDirectory(), code);
			if (code) {
				return Fail("could not prepare the world identity directory: " + code.message());
			}
			const std::string path = (LaneDirectory() / "persistent.identity").string();
			NetWorldIdentity first;
			if (!NetWorldIdentityFile::OpenForBoot(path, first, &error) || !first.IsValid()) {
				return Fail("first boot did not publish a world identity: " + error);
			}
			NetWorldIdentity peek;
			if (!NetWorldIdentityFile::Peek(path, peek, &error) || peek.worldId != first.worldId) {
				return Fail("world-id-did-not-survive-restart");
			}
			std::cout << "[net-world-identity-print-selftest] world-id=" << first.worldId << std::endl;
			return 0;
		}

		int TestIdentitySurvivesRestart() {
			std::string error;
			if (!ResetLane(&error)) {
				return Fail(error);
			}
			const std::string path = (LaneDirectory() / "persistent.identity").string();
			NetWorldIdentity first;
			if (!NetWorldIdentityFile::OpenForBoot(path, first, &error) || !first.IsValid()) {
				return Fail("first boot did not publish a world identity: " + error);
			}
			if (first.boot != 1 || first.round != 1) {
				return Fail("first boot did not advance incarnation and round");
			}
			NetWorldIdentity peek;
			if (!NetWorldIdentityFile::Peek(path, peek, &error) || peek != first) {
				return Fail("world-identity-record-did-not-round-trip: read back world " + peek.worldId + " boot " +
				            std::to_string(peek.boot) + " round " + std::to_string(peek.round) + ", wrote boot " +
				            std::to_string(first.boot) + " round " + std::to_string(first.round));
			}
			NetWorldIdentity second;
			if (!NetWorldIdentityFile::OpenForBoot(path, second, &error)) {
				return Fail("second boot could not reopen the world identity: " + error);
			}
			if (second.worldId != first.worldId) {
				return Fail("world-id-did-not-survive-restart");
			}
			if (second.boot != first.boot + 1 || second.round != first.round + 1) {
				return Fail("a restart did not advance boot and round before listen");
			}
			NetWorldIdentity decoded;
			if (!NetWorldIdentityFile::Decode(NetWorldIdentityFile::Encode(second), decoded, &error) || decoded != second) {
				return Fail("world-identity-record-did-not-round-trip: encode read back world " + decoded.worldId + " boot " +
				            std::to_string(decoded.boot) + " token \"" + decoded.directoryToken + "\", wrote boot " +
				            std::to_string(second.boot) + " token \"" + second.directoryToken + "\"");
			}
			// The directory row token is the world's own; a reboot resumes its row by presenting it.
			NetWorldIdentity tokened = second;
			tokened.directoryToken = "row-token-value";
			if (!NetWorldIdentityFile::Write(path, tokened, &error)) {
				return Fail("world-identity-record-did-not-round-trip: the token could not be written: " + error);
			}
			NetWorldIdentity third;
			if (!NetWorldIdentityFile::OpenForBoot(path, third, &error) || third.directoryToken != tokened.directoryToken) {
				return Fail("world-identity-record-did-not-round-trip: a reboot read token \"" + third.directoryToken +
				            "\", the stored token is \"" + tokened.directoryToken + "\"");
			}
			return 0;
		}

		int TestFrameLogAndOffer() {
			std::string error;
			NetWorldFrameLog log;
			if (!log.Append(MakeCommittedFrame(10), &error) || !log.Append(MakeCommittedFrame(11), &error)) {
				return Fail("committed tail refused an in-order frame: " + error);
			}
			if (log.Append(MakeCommittedFrame(13), &error)) {
				return Fail("committed tail accepted a gap");
			}
			if (!log.Covers(10) || !log.Covers(11) || log.Count() != 2) {
				return Fail("committed tail lost an in-order frame");
			}
			NetWorldCheckpointImage image;
			image.worldId = c_WorldId;
			image.boot = 1;
			image.round = 1;
			image.tick = 11;
			image.bytes = 32;
			image.digest = "abc";
			image.path = "Worlds/image.bin";
			NetWorldCheckpointImage decoded;
			if (!DecodeWorldJoinOffer(EncodeWorldJoinOffer(image), decoded, &error) || decoded.worldId != image.worldId || decoded.tick != image.tick || decoded.bytes != image.bytes) {
				return Fail("world join offer did not round-trip: " + error);
			}
			return 0;
		}

		int TestJoinPlaneAndLeave() {
			std::string error;
			NetWorldJoinHost host;
			if (!host.Configure(MakeWorldConfig(), MakeIdentity(), &error)) {
				return Fail("join plane refused a world config: " + error);
			}
			if (host.Membership().FreeSlots() != 1 || host.Membership().HeldSlots() != 0) {
				return Fail("a world boot held a slot before anyone joined");
			}
			if (!host.BeginJoin(7, 2, "alice", 1000, &error)) {
				return Fail("fresh join was refused: " + error);
			}
			const NetWorldJoinSession* session = host.FindSession(7);
			if (session == nullptr || session->spectator || session->assignedPeerId != 2 || session->openedAtMs != 1000) {
				return Fail("fresh join did not take the first free slot");
			}
			if (host.Membership().HeldSlots() != 1 || host.Membership().Slots().front().generation != 1) {
				return Fail("fresh join did not hold its slot under generation 1");
			}

			NetWorldCheckpointImage image;
			image.worldId = c_WorldId;
			image.boot = 1;
			image.round = 1;
			image.tick = 40;
			image.bytes = 64;
			image.digest = "d";
			image.path = "Worlds/image.bin";
			image.captureMs = 4.0;
			host.PublishImage(image);
			host.Metrics().NotePurity(40, 1, 1);
			if (!host.NoteTransferComplete(7, 64, &error)) {
				return Fail("transfer complete was refused: " + error);
			}
			uint64_t activation = 0;
			if (!host.NoteCatchUpProgress(7, 40, 20, 50, 80, &activation, &error) || activation != 80 + c_NetWorldActivationLeadFrames) {
				return Fail("catch-up did not announce activation at E: " + error);
			}
			if (host.DueActivation(activation) != nullptr) {
				return Fail("activation was due before the joiner applied through E-1");
			}
			if (!host.NoteCatchUpProgress(7, activation - 1, 1, 1, activation, nullptr, &error)) {
				return Fail("catch-up refused appliedThrough E-1: " + error);
			}
			if (host.DueActivation(activation - 2) != nullptr) {
				return Fail("activation was due before E-1");
			}
			if (host.DueActivation(activation - 1) == nullptr || !host.CompleteActivation(7, activation - 1, &error)) {
				return Fail("a due activation at E-1 was not ready: " + error);
			}
			if (host.FindSession(7)->phase != NetWorldJoinPhase::Active) {
				return Fail("activated joiner was not Active");
			}

			const uint32_t heldGeneration = host.Membership().Slots().front().generation;
			if (!host.Membership().Release(2, &error)) {
				return Fail("clean leave did not free the slot: " + error);
			}
			if (host.Membership().HeldSlots() != 0 || host.Membership().Slots().front().generation == heldGeneration) {
				return Fail("clean leave did not advance the slot generation");
			}
			if (!host.BeginJoin(8, 2, "alice", 2000, &error) || host.FindSession(8) == nullptr || host.FindSession(8)->assignedPeerId != 2) {
				return Fail("rejoin after a clean leave did not land in the running world");
			}

			if (!host.BeginJoin(9, 4, "carol", 3000, &error) || host.FindSession(9) == nullptr || !host.FindSession(9)->spectator) {
				return Fail("overflow did not spectate");
			}
			if (WorldJoinLobbyPeer(*host.FindSession(9)) != c_WorldSpectatorLobbyPeerFirst) {
				return Fail("first overflow spectator did not take lobby id 32");
			}
			if (!host.BeginJoin(10, 5, "dave", 3100, &error) || host.FindSession(10) == nullptr || !host.FindSession(10)->spectator) {
				return Fail("second overflow did not spectate");
			}
			if (WorldJoinLobbyPeer(*host.FindSession(10)) != c_WorldSpectatorLobbyPeerFirst + 1 ||
			    WorldJoinLobbyPeer(*host.FindSession(9)) == WorldJoinLobbyPeer(*host.FindSession(10))) {
				return Fail("two overflow spectators shared one lobby id");
			}
			if (host.ExpireStaleJoins(3000 + c_NetWorldJoinDeadlineMs + 1) == 0) {
				return Fail("a stalled bootstrap did not expire");
			}
			if (host.FindSession(9) == nullptr) {
				return Fail("a spectator was expired by the join deadline");
			}
			const std::string report = host.BuildReportJson();
			if (report.find("\"capture_p99_ms\"") == std::string::npos || report.find("\"catch_up_ratio\"") == std::string::npos) {
				return Fail("instrumentation report lost a baseline field");
			}
			return 0;
		}

		int TestOrdinaryStartStillRefusesEmptyRemotes() {
			LoopbackTransport transport;
			std::string error;
			if (!transport.StartHost(47111, &error)) {
				return Fail("loopback host: " + error);
			}
			NetLockstepCoordinator ordinary;
			NetLockstepConfig config;
			config.sessionId = 1;
			config.localPeerId = 1;
			config.peerCount = 2;
			config.timeoutMs = 1000000;
			if (ordinary.Start(transport, config, &error)) {
				return Fail(std::string("a two-peer empty remote was accepted: ") + error);
			}
			if (error != "lockstep peer identity is invalid") {
				return Fail(std::string("a two-peer empty remote was accepted: ") + error);
			}
			config.remotePeerId = 2;
			error.clear();
			if (ordinary.Start(transport, config, &error)) {
				return Fail(std::string("a two-peer empty remote was accepted: ") + error);
			}
			if (error != "lockstep peer identity is invalid") {
				return Fail(std::string("a two-peer empty remote was accepted: ") + error);
			}
			return 0;
		}

		int TestWorldStartsEmptyAndSurvivesLastLeave() {
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			std::string error;
			if (!hostTransport.StartHost(47112, &error) || !clientTransport.Connect("loopback", 47112, &error)) {
				return Fail("loopback pair: " + error);
			}

			NetLockstepCoordinator world;
			NetLockstepConfig worldConfig;
			worldConfig.sessionId = 2;
			worldConfig.localPeerId = 1;
			worldConfig.peerCount = 2;
			worldConfig.timeoutMs = 1000000;
			worldConfig.relayToOtherPeers = true;
			worldConfig.matchConfig = MakeWorldConfig();
			if (!world.Start(hostTransport, worldConfig, &error)) {
				return Fail("a persistent world refused to start with no members: " + error);
			}
			if (!world.IsRunning()) {
				return Fail("a persistent world with no members was not Running");
			}

			const uint64_t firstRequired = world.GetStats().nextFrame + c_NetWorldActivationLeadFrames;
			if (!world.AdmitWorldMember(2, 1, firstRequired, &error)) {
				return Fail("AdmitWorldMember refused a fresh member: " + error);
			}
			if (!world.IsWorldMember(2)) {
				return Fail("an activated member is not in the world's required set");
			}

			NetLockstepCoordinator client;
			NetLockstepConfig clientConfig;
			clientConfig.sessionId = 2;
			clientConfig.localPeerId = 2;
			clientConfig.remotePeerId = 1;
			clientConfig.remoteTransportPeerId = 1;
			clientConfig.peerCount = 2;
			clientConfig.timeoutMs = 1000000;
			clientConfig.matchConfig = MakeWorldConfig();
			if (!client.Start(clientTransport, clientConfig, &error)) {
				return Fail("world client start: " + error);
			}
			client.Leave("clean leave");
			for (int i = 0; i < 40; ++i) {
				world.Tick(static_cast<uint64_t>(i) * 5);
				client.Tick(static_cast<uint64_t>(i) * 5);
				hostTransport.AdvanceTimeMs(5);
				clientTransport.AdvanceTimeMs(5);
			}
			if (!world.IsRunning()) {
				return Fail("clean-leave-stopped-world");
			}
			return 0;
		}

		int TestOrdinaryLiveJoinAdmission() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			const NetH4Identity identity = MakeH4Identity();
			const std::vector<NetH4Seat> seats = {{0, 0, 0, false, 1, true}, {1, 1, 1, false, 2, false}};

			NetH4NewJoin join;
			join.identity = identity;
			join.displayName = "alice";
			join.txId.fill(9);

			NetSeatAuthRegistry registry;
			if (!registry.BeginHostedSession()) {
				return Fail("ordinary live-join registry did not arm");
			}
			NetReconnectHost host;
			host.Configure(&registry, 0x5741ULL, identity);
			host.SetSeatTable(seats, NetMatchMode::PvPSkirmish);
			host.SetLiveMatch(true);
			host.SetPersistentWorld(false);
			host.HandleMessage(11, join, 0);
			host.Tick(NetReconnectAdmission::c_DenialReleaseMs);
			bool refused = false;
			for (const NetH4Outbound& outbound: host.TakeOutbound()) {
				if (const auto* rejected = std::get_if<NetJoinRejected>(&outbound.payload)) {
					if (rejected->humanMessage == "the match is already in progress") {
						refused = true;
					}
				}
			}
			if (!refused) {
				return Fail("an ordinary NewJoin was accepted");
			}
			return 0;
		}

		int TestWorldLiveJoinAdmission() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			const NetH4Identity identity = MakeH4Identity();
			const std::vector<NetH4Seat> seats = {{0, 0, 0, false, 1, true}, {1, 1, 1, false, 2, false}};

			NetH4NewJoin join;
			join.identity = identity;
			join.displayName = "alice";
			join.txId.fill(9);

			NetSeatAuthRegistry registry;
			if (!registry.BeginHostedSession()) {
				return Fail("world live-join registry did not arm");
			}
			NetReconnectHost host;
			host.Configure(&registry, 0x5742ULL, identity);
			host.SetSeatTable(seats, NetMatchMode::PvPSkirmish);
			host.SetLiveMatch(true);
			host.SetPersistentWorld(true);
			host.HandleMessage(12, join, 0);
			host.Tick(NetReconnectAdmission::c_DenialReleaseMs);
			bool offered = false;
			for (const NetH4Outbound& outbound: host.TakeOutbound()) {
				if (std::holds_alternative<NetH4TicketOffer>(outbound.payload)) {
					offered = true;
				}
				if (const auto* rejected = std::get_if<NetJoinRejected>(&outbound.payload)) {
					if (rejected->humanMessage == "the match is already in progress") {
						return Fail("the match is already in progress");
					}
				}
			}
			if (!offered) {
				return Fail("a fresh join into a running world was not offered a seat");
			}
			return 0;
		}

	}

		int TestImageBlobRoundTrip() {
			NetWorldCheckpointImage image;
			image.worldId = c_WorldId;
			image.boot = 1;
			image.round = 1;
			image.tick = 40;
			image.bytes = 4;
			image.path = "Worlds/image.bin";
			const std::vector<uint8_t> archive = {0xCA, 0xFE, 0xBA, 0xBE};
			image.digest = DigestWorldJoinBytes(archive);
			image.bytes = archive.size();
			NetWorldFrameLog log;
			std::string error;
			if (!log.Append(MakeCommittedFrame(41), &error) || !log.Append(MakeCommittedFrame(42), &error)) {
				return Fail("blob tail refused an in-order frame: " + error);
			}
			std::vector<std::vector<uint8_t>> tail;
			(void)log.CopyFrom(41, 8, 1024, tail);
			std::vector<uint8_t> blob;
			if (!EncodeWorldJoinImageBlob(image, archive, tail, blob, &error) || !IsWorldJoinImageBlob(blob)) {
				return Fail("world join image did not encode: " + error);
			}
			NetWorldCheckpointImage decoded;
			std::vector<uint8_t> outArchive;
			std::vector<std::vector<uint8_t>> outTail;
			if (!DecodeWorldJoinImageBlob(blob, decoded, outArchive, outTail, &error)) {
				return Fail("world-join-transfer-digest-mismatch: " + error);
			}
			if (outArchive != archive || decoded.digest != image.digest || outTail != tail) {
				return Fail("world-join-transfer-bytes-in-not-equal-bytes-out");
			}
			std::vector<uint8_t> damaged = archive;
			damaged[0] ^= 1;
			std::vector<uint8_t> bad;
			NetWorldCheckpointImage badImage = image;
			badImage.digest = DigestWorldJoinBytes(archive);
			if (!EncodeWorldJoinImageBlob(badImage, damaged, tail, bad, &error)) {
				return Fail("damaged archive did not encode");
			}
			NetWorldCheckpointImage ignored;
			std::vector<uint8_t> ignoredArchive;
			std::vector<std::vector<uint8_t>> ignoredTail;
			if (DecodeWorldJoinImageBlob(bad, ignored, ignoredArchive, ignoredTail, &error)) {
				return Fail("world-join-transfer-digest-mismatch");
			}
			return 0;
		}

		int TestJoinerTransferPump() {
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			std::string error;
			if (!hostTransport.StartHost(47113, &error) || !clientTransport.Connect("loopback", 47113, &error)) {
				return Fail("loopback pair: " + error);
			}
			NetPeerId hostRemote = c_InvalidNetPeerId;
			NetPeerId clientRemote = c_InvalidNetPeerId;
			for (const NetTransportEvent& event: hostTransport.PollEvents()) {
				if (event.type == NetTransportEventType::PeerConnected) {
					hostRemote = event.peerId;
				}
			}
			for (const NetTransportEvent& event: clientTransport.PollEvents()) {
				if (event.type == NetTransportEventType::PeerConnected) {
					clientRemote = event.peerId;
				}
			}
			if (hostRemote == c_InvalidNetPeerId || clientRemote == c_InvalidNetPeerId) {
				return Fail("loopback peer connection events were not observed");
			}
			const std::vector<uint8_t> archive(80, 0x5A);
			NetWorldCheckpointImage image;
			image.worldId = c_WorldId;
			image.boot = 1;
			image.round = 1;
			image.tick = 12;
			image.bytes = archive.size();
			image.digest = DigestWorldJoinBytes(archive);
			image.path = "Worlds/pump.bin";
			std::vector<uint8_t> blob;
			if (!EncodeWorldJoinImageBlob(image, archive, {}, blob, &error)) {
				return Fail("pump blob did not encode: " + error);
			}
			NetLobbySession host;
			NetLobbySession client;
			NetLobbySessionConfig hostConfig;
			hostConfig.host = true;
			hostConfig.localPeerId = 1;
			hostConfig.remotePeerId = 2;
			hostConfig.remoteTransportPeerId = hostRemote;
			hostConfig.matchConfig = MakeWorldConfig();
			hostConfig.autoStart = false;
			NetLobbySessionConfig clientConfig;
			clientConfig.localPeerId = 2;
			clientConfig.remotePeerId = 1;
			clientConfig.remoteTransportPeerId = clientRemote;
			clientConfig.matchConfig = MakeWorldConfig();
			if (!host.Start(hostTransport, hostConfig, &error) || !client.Start(clientTransport, clientConfig, &error)) {
				return Fail("lobby start: " + error);
			}
			for (int i = 0; i < 8; ++i) {
				host.Tick(static_cast<uint64_t>(i) * 10);
				client.Tick(static_cast<uint64_t>(i) * 10);
				hostTransport.AdvanceTimeMs(10);
				clientTransport.AdvanceTimeMs(10);
			}
			if (!host.BindLateRemote(2, 1, &error)) {
				return Fail("bind late remote: " + error);
			}
			host.BeginStateTransferTo(2, blob);
			for (int i = 0; i < 40 && !client.HasCompleteStateTransfer(); ++i) {
				host.PumpOutgoingChunks();
				host.Tick(static_cast<uint64_t>(100 + i) * 10);
				client.Tick(static_cast<uint64_t>(100 + i) * 10);
				hostTransport.AdvanceTimeMs(10);
				clientTransport.AdvanceTimeMs(10);
			}
			if (!client.HasCompleteStateTransfer()) {
				return Fail("world-join-transfer-did-not-complete");
			}
			const std::vector<uint8_t> received = client.TakeReceivedState();
			if (received != blob) {
				return Fail("world-join-transfer-bytes-in-not-equal-bytes-out");
			}
			NetWorldCheckpointImage decoded;
			std::vector<uint8_t> outArchive;
			std::vector<std::vector<uint8_t>> outTail;
			if (!DecodeWorldJoinImageBlob(received, decoded, outArchive, outTail, &error) || outArchive != archive ||
			    decoded.digest != image.digest) {
				return Fail("world-join-transfer-digest-mismatch: " + error);
			}
			return 0;
		}

		int TestCatchUpJoinerReport() {
			std::string error;
			NetWorldFrameLog log;
			if (!log.Append(MakeCommittedFrame(41), &error) || !log.Append(MakeCommittedFrame(42), &error) ||
			    !log.Append(MakeCommittedFrame(43), &error)) {
				return Fail("catch-up log refused an in-order frame: " + error);
			}
			std::vector<std::vector<uint8_t>> copied;
			uint64_t lastCopied = 0;
			if (log.CopyFrom(41, 2, 1024, copied, &lastCopied) != 2 || lastCopied != 42) {
				return Fail("CopyFrom stamped the log tip instead of the last copied frame");
			}

			WorldLobbyPair pair;
			if (!pair.Open(47116, &error)) {
				return Fail("catch-up lobby pair: " + error);
			}
			NetWorldJoinHost host;
			if (!host.Configure(MakeWorldConfig(), MakeIdentity(), &error) || !host.BeginJoin(7, 2, "alice", 1000, &error)) {
				return Fail("catch-up host did not open a join: " + error);
			}
			NetWorldCheckpointImage image;
			image.worldId = c_WorldId;
			image.boot = 1;
			image.round = 1;
			image.tick = 40;
			image.bytes = 8;
			image.digest = "d";
			image.path = "Worlds/image.bin";
			host.PublishImage(image);
			if (!host.NoteTransferComplete(7, 8, &error)) {
				return Fail("catch-up transfer complete refused: " + error);
			}
			// The world keeps committing while the joiner replays; these are the frames it owes it.
			for (uint64_t frame = 41; frame <= 43; ++frame) {
				if (!host.Tail().Append(MakeCommittedFrame(frame), &error)) {
					return Fail("catch-up tail refused a committed frame: " + error);
				}
			}
			if (!pair.host.BindLateRemote(2, pair.hostRemote, &error)) {
				return Fail("catch-up bind: " + error);
			}
			const NetWorldJoinSession* bootstrap = host.FindSession(7);
			if (bootstrap == nullptr) {
				return Fail("catch-up host lost its bootstrap");
			}
			NetMatchService::SendWorldJoinTailTo(pair.host, host, *bootstrap);
			pair.Pump(8);

			const uint64_t e = 44;
			NetWorldCatchUpClient catchUp;
			catchUp.active = true;
			catchUp.snapshotTick = 40;
			catchUp.appliedThrough = 40;
			if (!ScenarioRunner::InstallWorldCatchUp(40, {}, &error)) {
				return Fail("catch-up install: " + error);
			}
			ScenarioRunner::SetWorldCatchUpActivation(e);
			// The joiner's own step takes the tail off the wire; nothing here hands it frames.
			NetMatchService::StepWorldJoinCatchUpClient(pair.client, catchUp);
			int budget = ScenarioRunner::c_WorldCatchUpTicksPerRealFrame;
			uint64_t simTick = 40;
			while (ScenarioRunner::WorldCatchUpMayGrant(simTick + 1, budget)) {
				++simTick;
				--budget;
				NetLockstepReadyFrame ready;
				if (!ScenarioRunner::TakeWorldCatchUpReadyFrame(simTick, ready, &error) || ready.frame != simTick) {
					return Fail("appliedThrough-did-not-reach-E-minus-1: tick " + std::to_string(simTick) + " was granted with no committed frame");
				}
			}
			if (simTick != e - 1 || ScenarioRunner::WorldCatchUpAppliedThrough() != e - 1) {
				return Fail("appliedThrough-did-not-reach-E-minus-1: applied " + std::to_string(ScenarioRunner::WorldCatchUpAppliedThrough()) +
				            " of " + std::to_string(e - 1));
			}
			// E-1 applied: the sim is HELD here, not released - the round has not started this joiner.
			if (!ScenarioRunner::WorldCatchUpHolding() || ScenarioRunner::WorldCatchUpMayGrant(e, 1)) {
				return Fail("catch-up-released-the-sim-before-its-round: holding " +
				            std::string(ScenarioRunner::WorldCatchUpHolding() ? "true" : "false") + ", tick " +
				            std::to_string(e) + " granted " + std::string(ScenarioRunner::WorldCatchUpMayGrant(e, 1) ? "true" : "false"));
			}
			// The value the joiner puts on the wire, read off the host's lobby - not one built here.
			NetMatchService::StepWorldJoinCatchUpClient(pair.client, catchUp);
			pair.Pump(6);
			const NetLobbySession::WorldJoinReport sent = pair.host.TakeWorldJoinReport();
			if (!sent.pending || sent.kind != c_NetWorldReportCatchUp) {
				return Fail("appliedThrough-did-not-reach-E-minus-1: the joiner sent no catch-up report");
			}
			const uint64_t nowFrame = 80;
			const NetPeerId connection = NetMatchService::ResolveWorldReportConnection(host, {}, sent.fromPeer);
			if (connection != 7) {
				return Fail("appliedThrough-did-not-reach-E-minus-1: the host could not place lobby peer " + std::to_string(static_cast<int>(sent.fromPeer)));
			}
			NetMatchService::ApplyWorldJoinReport(pair.host, host, sent, connection, nowFrame, 2000);
			const uint64_t stored = host.FindSession(7)->acknowledgedThrough;
			if (stored != e - 1) {
				return Fail("appliedThrough-did-not-reach-E-minus-1: the host stored " + std::to_string(stored) + ", the joiner applied " +
				            std::to_string(e - 1) + ", the host frame is " + std::to_string(nowFrame));
			}
			if (host.FindSession(7)->activationTick != nowFrame + c_NetWorldActivationLeadFrames) {
				return Fail("catch-up did not announce E: " + std::to_string(host.FindSession(7)->activationTick));
			}
			return 0;
		}

		int TestCatchUpStopsAtTailGap() {
			std::string error;
			std::vector<NetLockstepFrame> tail = {MakeCommittedFrame(41), MakeCommittedFrame(42), MakeCommittedFrame(44)};
			if (!ScenarioRunner::InstallWorldCatchUp(40, tail, &error)) {
				return Fail("gapped catch-up install: " + error);
			}
			ScenarioRunner::SetWorldCatchUpActivation(50);
			int budget = ScenarioRunner::c_WorldCatchUpTicksPerRealFrame;
			uint64_t simTick = 40;
			while (ScenarioRunner::WorldCatchUpMayGrant(simTick + 1, budget)) {
				++simTick;
				--budget;
				NetLockstepReadyFrame ready;
				if (!ScenarioRunner::TakeWorldCatchUpReadyFrame(simTick, ready, &error)) {
					return Fail("catch-up-ran-past-the-tail: tick " + std::to_string(simTick) + " was granted and the tail holds no frame for it");
				}
			}
			if (simTick != 42) {
				return Fail("catch-up-ran-past-the-tail: the sim clock reached " + std::to_string(simTick) + " over a tail that ends at 42 before the hole");
			}
			if (ScenarioRunner::WorldCatchUpAppliedThrough() != 42) {
				return Fail("catch-up-ran-past-the-tail: applied through " + std::to_string(ScenarioRunner::WorldCatchUpAppliedThrough()) + ", the tail holds 42");
			}
			// The frame behind the hole is still held, so the joiner resumes when 43 arrives.
			if (!ScenarioRunner::WorldCatchUpHasFrame(44)) {
				return Fail("catch-up-ran-past-the-tail: the frame behind the hole was consumed");
			}
			return 0;
		}

		int TestCatchUpKeepsItsCeiling() {
			std::string error;
			std::vector<NetLockstepFrame> tail;
			const int ceiling = ScenarioRunner::c_WorldCatchUpTicksPerRealFrame;
			for (int i = 1; i <= ceiling + 4; ++i) {
				tail.push_back(MakeCommittedFrame(40 + static_cast<uint64_t>(i)));
			}
			if (!ScenarioRunner::InstallWorldCatchUp(40, tail, &error)) {
				return Fail("ceiling catch-up install: " + error);
			}
			ScenarioRunner::SetWorldCatchUpActivation(40 + static_cast<uint64_t>(ceiling) + 5);
			// The predicate's own contract: no ticks left this frame is a refusal whatever the tail holds.
			if (ScenarioRunner::WorldCatchUpMayGrant(41, 0) || !ScenarioRunner::WorldCatchUpMayGrant(41, 1)) {
				return Fail("catch-up-exceeded-the-ceiling: the grant predicate answered " +
				            std::string(ScenarioRunner::WorldCatchUpMayGrant(41, 0) ? "true" : "false") + " with 0 ticks left and " +
				            std::string(ScenarioRunner::WorldCatchUpMayGrant(41, 1) ? "true" : "false") + " with 1");
			}
			// The budget is production's: Main.cpp opens the frame here and every grant is taken here.
			ScenarioRunner::BeginWorldCatchUpFrame();
			uint64_t simTick = 40;
			int granted = 0;
			while (ScenarioRunner::TakeWorldCatchUpGrant(simTick + 1)) {
				++simTick;
				++granted;
				NetLockstepReadyFrame ready;
				if (!ScenarioRunner::TakeWorldCatchUpReadyFrame(simTick, ready, &error)) {
					return Fail("catch-up-exceeded-the-ceiling: tick " + std::to_string(simTick) + " had no committed frame");
				}
			}
			if (granted != ceiling) {
				return Fail("catch-up-exceeded-the-ceiling: " + std::to_string(granted) + " ticks in one real frame, the ceiling is " + std::to_string(ceiling));
			}
			// A second real frame opens a second budget; the tail still holds four frames.
			ScenarioRunner::BeginWorldCatchUpFrame();
			if (!ScenarioRunner::TakeWorldCatchUpGrant(simTick + 1)) {
				return Fail("catch-up-exceeded-the-ceiling: the next real frame granted no tick over a tail that holds " +
				            std::to_string(40 + ceiling + 4) + " and a sim clock at " + std::to_string(simTick));
			}
			return 0;
		}

		/// F1/F2/F3 of the r6 correctness read: the stop gate must not swallow the catch-up window, the
		/// sim is held from the tail's last frame until the joiner's own round runs, and the catch-up is
		/// released the moment it does.
		int TestCatchUpHoldsUntilItsRoundRuns() {
			std::string error;
			const uint64_t e = 44;
			std::vector<NetLockstepFrame> tail = {MakeCommittedFrame(41), MakeCommittedFrame(42), MakeCommittedFrame(43)};
			if (!ScenarioRunner::InstallWorldCatchUp(40, tail, &error)) {
				return Fail("hold catch-up install: " + error);
			}
			ScenarioRunner::SetWorldCatchUpActivation(e);

			LoopbackTransport holdTransport;
			if (!holdTransport.StartHost(47120, &error)) {
				return Fail("hold loopback: " + error);
			}
			NetLockstepCoordinator joinerRound;
			NetLockstepConfig joinerConfig;
			joinerConfig.sessionId = 9;
			joinerConfig.localPeerId = 2;
			joinerConfig.remotePeerId = 1;
			joinerConfig.remoteTransportPeerId = 1;
			joinerConfig.peerCount = 2;
			joinerConfig.timeoutMs = 1000000;
			joinerConfig.startFrame = e;
			joinerConfig.matchConfig = MakeWorldConfig();
			if (!joinerRound.Start(holdTransport, joinerConfig, &error)) {
				return Fail("hold joiner coordinator: " + error);
			}
			// The joiner's coordinator is attached and NOT running: that is the whole catch-up window.
			ScenarioRunner::SetLockstepCoordinator(&joinerRound);
			struct CoordinatorScope {
				~CoordinatorScope() { ScenarioRunner::SetLockstepCoordinator(nullptr); }
			} coordinatorScope;
			if (ScenarioRunner::IsLockstepControllerSyncActive() || !ScenarioRunner::HasLockstepCoordinator()) {
				return Fail("lockstep-stop-gate-swallowed-the-catch-up: the joiner's coordinator is running " +
				            std::string(ScenarioRunner::IsLockstepControllerSyncActive() ? "true" : "false") + " and attached " +
				            std::string(ScenarioRunner::HasLockstepCoordinator() ? "true" : "false"));
			}
			if (ScenarioRunner::LockstepStopHoldsControllers()) {
				return Fail("lockstep-stop-gate-swallowed-the-catch-up: the stop gate held the controller update at tick 41 "
				            "while the tail still holds frames through 43");
			}
			if (ScenarioRunner::OfflineCommandsDriveTick()) {
				return Fail("lockstep-stop-gate-swallowed-the-catch-up: a catch-up tick ran the offline command paths");
			}
			ScenarioRunner::BeginWorldCatchUpFrame();
			uint64_t simTick = 40;
			while (ScenarioRunner::TakeWorldCatchUpGrant(simTick + 1)) {
				++simTick;
				NetLockstepReadyFrame ready;
				if (!ScenarioRunner::TakeWorldCatchUpReadyFrame(simTick, ready, &error)) {
					return Fail("catch-up-released-the-sim-before-its-round: tick " + std::to_string(simTick) + " had no committed frame");
				}
			}
			if (simTick != e - 1) {
				return Fail("catch-up-released-the-sim-before-its-round: the tail stopped at " + std::to_string(simTick) +
				            ", E-1 is " + std::to_string(e - 1));
			}
			// The tail is exhausted at E-1 and the round has not started: ticks E and E+1 are refused.
			ScenarioRunner::BeginWorldCatchUpFrame();
			if (!ScenarioRunner::WorldCatchUpActive() || !ScenarioRunner::WorldCatchUpHolding() ||
			    ScenarioRunner::TakeWorldCatchUpGrant(e) || ScenarioRunner::TakeWorldCatchUpGrant(e + 1)) {
				return Fail("catch-up-released-the-sim-before-its-round: active " +
				            std::string(ScenarioRunner::WorldCatchUpActive() ? "true" : "false") + ", holding " +
				            std::string(ScenarioRunner::WorldCatchUpHolding() ? "true" : "false") + ", tick " +
				            std::to_string(e) + " granted " + std::string(ScenarioRunner::TakeWorldCatchUpGrant(e) ? "true" : "false"));
			}
			if (ScenarioRunner::LockstepStopHoldsControllers()) {
				return Fail("lockstep-stop-gate-swallowed-the-catch-up: the held window was read as a stopped round");
			}

			// The coordinator runs: the production release ends the catch-up and normal pacing resumes.
			NetWorldCatchUpClient catchUp;
			catchUp.active = true;
			catchUp.snapshotTick = 40;
			catchUp.appliedThrough = e - 1;
			catchUp.activationTick = e;
			if (NetMatchService::ReleaseWorldCatchUpOnceRunning(false, catchUp) || !catchUp.active) {
				return Fail("world-catch-up-outlived-its-round: a coordinator that is not running released the catch-up");
			}
			if (!NetMatchService::ReleaseWorldCatchUpOnceRunning(true, catchUp)) {
				return Fail("world-catch-up-outlived-its-round: a running coordinator did not release the catch-up");
			}
			if (catchUp.active || catchUp.appliedThrough != 0 || ScenarioRunner::WorldCatchUpActive() || ScenarioRunner::WorldCatchUpHolding()) {
				return Fail("world-catch-up-outlived-its-round: client active " + std::string(catchUp.active ? "true" : "false") +
				            " appliedThrough " + std::to_string(catchUp.appliedThrough) + ", runner active " +
				            std::string(ScenarioRunner::WorldCatchUpActive() ? "true" : "false"));
			}
			// With the catch-up gone the stop gate owns the window again: the round is what drives ticks.
			if (!ScenarioRunner::LockstepStopHoldsControllers() || !ScenarioRunner::OfflineCommandsDriveTick()) {
				return Fail("world-catch-up-outlived-its-round: after the release the stop gate answered " +
				            std::string(ScenarioRunner::LockstepStopHoldsControllers() ? "true" : "false") +
				            " and the offline path " + std::string(ScenarioRunner::OfflineCommandsDriveTick() ? "true" : "false"));
			}
			return 0;
		}

		/// An Activate never seats a brain another member holds, and a capacity slot the roster does not
		/// name binds no brain at all.
		int TestActivateNeverTakesAnotherMembersBrain() {
			NetMatchConfig config = MakeWorldConfig();
			// Co-op: every human seat is team 0, so two members share the team an Activate names.
			config.players = {
			    NetMatchPlayerSlot{1, 0, true, "World"},
			    NetMatchPlayerSlot{2, 0, false, "First"},
			    NetMatchPlayerSlot{3, 0, false, "Second"},
			};
			config.peerCount = 3;
			NetWorldJoinSession first;
			first.connection = 7;
			first.assignedPeerId = 2;
			first.team = 0;
			first.activationTick = 120;
			NetWorldJoinSession second = first;
			second.connection = 8;
			second.assignedPeerId = 3;
			const NetGameWorldTransition firstActivate = BuildWorldActivateTransition(first, config, 1);
			const NetGameWorldTransition secondActivate = BuildWorldActivateTransition(second, config, 1);
			if (firstActivate.player != 0 || secondActivate.player != 1) {
				return Fail("activate-bound-a-seat-the-roster-does-not-name: the two co-op members resolved to players " +
				            std::to_string(firstActivate.player) + " and " + std::to_string(secondActivate.player));
			}
			// The first member holds the brain the world already has; the second must not take it.
			std::vector<NetWorldBrainCandidate> brains = {NetWorldBrainCandidate{900, 0, firstActivate.peerId}};
			if (ChooseWorldActivateBrain(brains, secondActivate) != 0) {
				return Fail("activate-took-another-members-brain: peer " + std::to_string(static_cast<int>(secondActivate.peerId)) +
				            " was seated on actor " + std::to_string(ChooseWorldActivateBrain(brains, secondActivate)) +
				            ", held by peer " + std::to_string(static_cast<int>(firstActivate.peerId)));
			}
			// A departed seat's brain is unowned, so the world hands it to the arriving member.
			brains.push_back(NetWorldBrainCandidate{901, 0, 0});
			if (ChooseWorldActivateBrain(brains, secondActivate) != 901) {
				return Fail("activate-took-another-members-brain: the unowned brain of team 0 was not chosen, got " +
				            std::to_string(ChooseWorldActivateBrain(brains, secondActivate)));
			}
			// Another team's unowned brain is not this transition's to take.
			const std::vector<NetWorldBrainCandidate> otherTeam = {NetWorldBrainCandidate{902, 1, 0}};
			if (ChooseWorldActivateBrain(otherTeam, secondActivate) != 0) {
				return Fail("activate-took-another-members-brain: a team 1 brain was seated for a team " +
				            std::to_string(secondActivate.team) + " activation");
			}
			// A capacity slot the roster does not name owns no activity player, so it binds no brain.
			NetWorldJoinSession capacity = first;
			capacity.connection = 9;
			capacity.assignedPeerId = 4;
			const NetGameWorldTransition unnamed = BuildWorldActivateTransition(capacity, config, 1);
			if (unnamed.player != -1 || WorldTransitionBindsBrain(unnamed, true)) {
				return Fail("activate-bound-a-seat-the-roster-does-not-name: peer 4 resolved to player " +
				            std::to_string(unnamed.player) + " and binds " +
				            std::string(WorldTransitionBindsBrain(unnamed, true) ? "a brain" : "none"));
			}
			return 0;
		}

		/// The host's bootstrap plane: a bootstrap that can never start is ended, a refused image is not
		/// a start, and the host never keeps a joiner's tail bytes.
		/// The image a world offers is what the autosave WRITER finished, never a file the sim thread
		/// reads back while the zip is still being written.
		int TestWorldImagePublishedFromTheWriter() {
			std::string error;
			const std::vector<uint8_t> archive = {0x50, 0x4B, 0x03, 0x04, 0x11, 0x22, 0x33, 0x44};
			ActivityMan::CompletedAutosave finished;
			finished.serial = 1;
			finished.tick = 90;
			finished.path = "Autosaves/world-90.ccsave";
			finished.bytes = archive.size();
			finished.digest = DigestWorldJoinBytes(archive);
			finished.archive = std::make_shared<const std::vector<uint8_t>>(archive);
			const NetWorldCheckpointImage image = NetMatchService::WorldImageFromAutosave(finished, MakeIdentity(), MakeWorldConfig(), 3, 1.5);
			if (!image.IsValid() || image.tick != finished.tick || image.bytes != archive.size() ||
			    image.digest != DigestWorldJoinBytes(archive) || image.path != finished.path) {
				return Fail("world-image-published-before-the-archive-was-written: the offer reads tick " +
				            std::to_string(image.tick) + " bytes " + std::to_string(image.bytes) + " digest \"" + image.digest +
				            "\" for an archive of " + std::to_string(archive.size()) + " bytes whose digest is \"" +
				            DigestWorldJoinBytes(archive) + "\"");
			}
			// A capture the writer has not finished publishes nothing: no entry, or an entry with no
			// buffer, or one whose byte count does not match what it holds.
			ActivityMan::CompletedAutosave capturing;
			capturing.tick = 91;
			capturing.path = finished.path;
			capturing.bytes = 4096;
			if (NetMatchService::WorldImageFromAutosave(capturing, MakeIdentity(), MakeWorldConfig(), 3, 1.5).IsValid()) {
				return Fail("world-image-publish-read-the-sim-thread: a capture with no finished archive was published as an image");
			}
			ActivityMan::CompletedAutosave partial = finished;
			partial.bytes = archive.size() + 1;
			if (NetMatchService::WorldImageFromAutosave(partial, MakeIdentity(), MakeWorldConfig(), 3, 1.5).IsValid()) {
				return Fail("world-image-publish-read-the-sim-thread: an entry whose byte count disagrees with its buffer was published");
			}
			// With nothing published a bootstrap waits in SnapshotTransfer and nothing is counted stalled.
			NetWorldJoinHost host;
			if (!host.Configure(MakeWorldConfig(), MakeIdentity(), &error) || !host.BeginJoin(7, 2, "alice", 1000, &error)) {
				return Fail("world image host did not open a join: " + error);
			}
			const NetWorldJoinSession* waiting = host.FindSession(7);
			if (host.Image().IsValid() || waiting == nullptr || waiting->phase != NetWorldJoinPhase::SnapshotTransfer ||
			    host.Metrics().BootstrapStalls() != 0) {
				return Fail("world-image-publish-read-the-sim-thread: with no finished archive the bootstrap is " +
				            std::string(waiting == nullptr ? "gone" : NetWorldJoinPhaseName(waiting->phase)) + " with " +
				            std::to_string(host.Metrics().BootstrapStalls()) + " stalls counted");
			}
			return 0;
		}

		int TestHostBootstrapRefusals() {
			std::string error;
			NetWorldJoinHost host;
			if (!host.Configure(MakeWorldConfig(), MakeIdentity(), &error)) {
				return Fail("bootstrap host did not configure: " + error);
			}
			NetWorldCheckpointImage image;
			image.worldId = c_WorldId;
			image.boot = 1;
			image.round = 1;
			image.tick = 40;
			image.bytes = 8;
			image.digest = "d";
			image.path = "Worlds/image.bin";
			host.PublishImage(image);
			// The spectator pool is 16 ids wide; the bootstrap past the last one holds none.
			NetPeerId overflow = c_InvalidNetPeerId;
			int spectators = 0;
			for (int index = 0; index < static_cast<int>(c_WorldSpectatorLobbyCap) + 4 && overflow == c_InvalidNetPeerId; ++index) {
				const NetPeerId connection = static_cast<NetPeerId>(100 + index);
				if (!host.BeginJoin(connection, 0, "spectator", 1000, &error)) {
					return Fail("bootstrap spectator " + std::to_string(index) + " did not open: " + error);
				}
				const NetWorldJoinSession* opened = host.FindSession(connection);
				if (opened == nullptr) {
					return Fail("bootstrap spectator " + std::to_string(index) + " left no session");
				}
				if (!opened->spectator) {
					continue;
				}
				++spectators;
				if (opened->spectatorLobbyPeer == 0) {
					overflow = connection;
				}
			}
			if (overflow == c_InvalidNetPeerId) {
				return Fail("bootstrap-read-the-archive-it-cannot-send: " + std::to_string(spectators) +
				            " spectators all held a lobby id, the pool is " + std::to_string(c_WorldSpectatorLobbyCap));
			}
			std::string reason;
			if (NetMatchService::WorldBootstrapCanStart(*host.FindSession(overflow), &reason)) {
				return Fail("bootstrap-read-the-archive-it-cannot-send: connection " + std::to_string(overflow) +
				            " with no lobby id was reported startable");
			}
			if (reason.empty()) {
				return Fail("bootstrap-read-the-archive-it-cannot-send: the refusal printed no reason");
			}
			host.Metrics().NoteBootstrapStall();
			host.CancelJoin(overflow, reason);
			if (host.FindSession(overflow) != nullptr || host.Metrics().BootstrapStalls() != 1) {
				return Fail("bootstrap-read-the-archive-it-cannot-send: the bootstrap survived with " +
				            std::to_string(host.Metrics().BootstrapStalls()) + " stalls counted, reason \"" + reason + "\"");
			}

			WorldLobbyPair pair;
			if (!pair.Open(47121, &error)) {
				return Fail("bootstrap lobby pair: " + error);
			}
			NetWorldJoinHost second;
			if (!second.Configure(MakeWorldConfig(), MakeIdentity(), &error) || !second.BeginJoin(7, 2, "alice", 1000, &error)) {
				return Fail("bootstrap second host did not open a join: " + error);
			}
			second.PublishImage(image);
			// An unknown remote is a refusal, and a refusal must not mark the transfer started.
			const NetLobbyStateTransfer refused = pair.host.BeginStateTransferToPeer(60, std::vector<uint8_t>(64, 1));
			if (refused != NetLobbyStateTransfer::Refused) {
				return Fail("refused-transfer-was-marked-started: an unbound peer answered " +
				            std::to_string(static_cast<int>(refused)));
			}
			if (NetMatchService::NoteImageTransferOutcome(refused, pair.host, second, 7, 40) || second.FindSession(7)->transferStarted) {
				return Fail("refused-transfer-was-marked-started: the bootstrap reads started " +
				            std::string(second.FindSession(7)->transferStarted ? "true" : "false") + " after a refusal");
			}
			// A MEMBER's image walks the real gate: its own lobby messages marked it up during Open.
			const NetLobbyStateTransfer member = pair.host.BeginStateTransferToPeer(2, std::vector<uint8_t>(64, 2));
			if (member != NetLobbyStateTransfer::Started) {
				return Fail("refused-transfer-was-marked-started: a bound member answered " +
				            std::to_string(static_cast<int>(member)) + " instead of Started");
			}
			if (!NetMatchService::NoteImageTransferOutcome(member, pair.host, second, 7, 40) || !second.FindSession(7)->transferStarted) {
				return Fail("refused-transfer-was-marked-started: a taken transfer left the bootstrap unstarted");
			}
			pair.Pump(24);
			if (!pair.host.IsRemoteLobbyUp(2) || pair.client.TakeReceivedState().empty()) {
				return Fail("member-image-never-left-the-host: peer 2 lobby-up " +
				            std::string(pair.host.IsRemoteLobbyUp(2) ? "true" : "false") + ", received bytes " +
				            std::to_string(pair.client.PeekReceivedState().size()) + " after 24 pumps");
			}
			// An overflow spectator's reserved id never reaches a lobby payload, so it is never marked
			// up. Its image goes out by transport all the same.
			if (!pair.host.BindWorldTransferRemote(c_WorldSpectatorLobbyPeerFirst, pair.hostRemote, &error)) {
				return Fail("bootstrap spectator bind: " + error);
			}
			const NetLobbyStateTransfer spectatorImage = pair.host.BeginStateTransferToPeer(c_WorldSpectatorLobbyPeerFirst, std::vector<uint8_t>(64, 3));
			if (spectatorImage != NetLobbyStateTransfer::Started) {
				return Fail("spectator-image-never-left-the-host: the reserved id answered " +
				            std::to_string(static_cast<int>(spectatorImage)) + " instead of Started");
			}
			pair.Pump(24);
			if (pair.host.IsRemoteLobbyUp(c_WorldSpectatorLobbyPeerFirst) || pair.client.PeekReceivedState().empty()) {
				return Fail("spectator-image-never-left-the-host: reserved id " +
				            std::to_string(static_cast<int>(c_WorldSpectatorLobbyPeerFirst)) + " lobby-up " +
				            std::string(pair.host.IsRemoteLobbyUp(c_WorldSpectatorLobbyPeerFirst) ? "true" : "false") +
				            ", received bytes " + std::to_string(pair.client.PeekReceivedState().size()) + " after 24 pumps");
			}

			// A retried transfer does not send the seat's match config again: only the first try does.
			if (!second.NoteMatchConfigSent(7) || second.NoteMatchConfigSent(7)) {
				return Fail("bootstrap-resent-the-match-config: the second attempt for connection 7 sent it again");
			}

			// The host never drains a tail, so a tail chunk aimed at it must not be kept.
			NetLobbyStateChunk tailChunk;
			tailChunk.transferId = c_NetWorldTailTransferId;
			tailChunk.totalBytes = 4;
			tailChunk.chunkIndex = 0;
			tailChunk.chunkCount = 1;
			tailChunk.bytes = {1, 2, 3, 4};
			if (!pair.client.SendPayload(tailChunk, &error)) {
				return Fail("bootstrap tail chunk did not send: " + error);
			}
			pair.Pump(8);
			if (!pair.host.TakePendingTailBytes().empty()) {
				return Fail("host-kept-a-joiner-tail-chunk: the host buffered a tail chunk from a bound remote");
			}
			return 0;
		}

		int TestTakeDoesNotPumpTheSession() {
			std::string error;
			std::vector<NetLockstepFrame> tail = {MakeCommittedFrame(41)};
			if (!ScenarioRunner::InstallWorldCatchUp(40, tail, &error)) {
				return Fail("pump catch-up install: " + error);
			}
			ScenarioRunner::SetWorldCatchUpActivation(60);
			bool pumped = false;
			ScenarioRunner::SetSessionPump([&pumped]() { pumped = true; });
			NetLockstepReadyFrame ready;
			const bool took = ScenarioRunner::TakeWorldCatchUpReadyFrame(41, ready, &error);
			ScenarioRunner::SetSessionPump(std::function<void()>{});
			if (!took || ready.frame != 41) {
				return Fail("the catch-up did not apply its committed frame: " + error);
			}
			if (pumped) {
				return Fail("take-pumped-the-session: applying a committed tail frame ran the session pump on the sim thread");
			}
			return 0;
		}

		int TestImageWatermarkSurvivesAnEmptyCopy() {
			std::string error;
			NetWorldJoinHost host;
			if (!host.Configure(MakeWorldConfig(), MakeIdentity(), &error) || !host.BeginJoin(7, 2, "alice", 1000, &error)) {
				return Fail("watermark host did not open a join: " + error);
			}
			NetWorldCheckpointImage image;
			image.worldId = c_WorldId;
			image.boot = 1;
			image.round = 1;
			image.tick = 40;
			image.bytes = 8;
			image.digest = "d";
			image.path = "Worlds/image.bin";
			host.PublishImage(image);
			if (!host.NoteTransferStarted(7, 0x1234, 3, 0)) {
				return Fail("watermark transfer start refused");
			}
			if (host.FindSession(7)->deliveredThrough != image.tick) {
				return Fail("image-watermark-wiped: deliveredThrough " + std::to_string(host.FindSession(7)->deliveredThrough) +
				            " after a transfer that copied no tail, the image stands at " + std::to_string(image.tick));
			}
			if (!host.NoteTransferStarted(7, 0x1234, 3, 42) || host.FindSession(7)->deliveredThrough != 42) {
				return Fail("image-watermark-wiped: a copied tail frame did not move deliveredThrough");
			}
			return 0;
		}

		int TestTicketRejoinTargetsTheWorld() {
			NetH4TicketRecord record;
			record.epoch.fill(7);
			record.stableSeat = 3;
			record.holderGeneration = 2;
			record.credential.fill(9);
			record.hostSessionId = 0x5151ULL;
			record.hostAddress = "198.51.100.7:42124";
			record.issuedAtUnixMs = 1000;
			record.persistentWorld = true;
			const NetMatchServiceRequest world = NetMatchService::BuildTicketRejoinRequest(record, "alice", false);
			if (!world.persistentWorld || world.activityPreset != "Persistent World") {
				return Fail("ticket-rejoin-did-not-target-the-world: persistentWorld " + std::string(world.persistentWorld ? "true" : "false") +
				            ", preset \"" + world.activityPreset + "\" from a world ticket with no live world flags");
			}
			if (world.address != record.hostAddress || world.host) {
				return Fail("ticket-rejoin-did-not-target-the-world: the request left the ticket's host");
			}
			NetH4TicketRecord ordinary = record;
			ordinary.persistentWorld = false;
			if (NetMatchService::BuildTicketRejoinRequest(ordinary, "alice", false).persistentWorld) {
				return Fail("an ordinary ticket rejoin targeted a world");
			}
			// The flag has to survive the record's own round trip: a relaunch reads it off disk.
			record.directorySessionId = "sess-world-1";
			record.recordVersion = NetReconnectTicketStore::RecordVersionFor(record.persistentWorld);
			if (record.recordVersion != NetReconnectTicketStore::c_RecordVersion) {
				return Fail("ticket-rejoin-did-not-target-the-world: a world ticket asked for record version " +
				            std::to_string(record.recordVersion) + ", the world body is version " +
				            std::to_string(NetReconnectTicketStore::c_RecordVersion));
			}
			std::vector<uint8_t> bytes;
			if (!NetReconnectTicketStore::Serialize(record, bytes)) {
				return Fail("ticket-rejoin-did-not-target-the-world: the world record did not serialize");
			}
			NetH4TicketRecord decoded;
			if (!NetReconnectTicketStore::Deserialize(bytes, decoded) || !decoded.persistentWorld || !(decoded == record)) {
				return Fail("ticket-rejoin-did-not-target-the-world: the stored record read back world " +
				            std::string(decoded.persistentWorld ? "true" : "false") + " version " +
				            std::to_string(decoded.recordVersion) + " session \"" + decoded.directorySessionId + "\"");
			}
			// An ordinary ticket on this build stops at the directory session id: no build is handed a
			// version it cannot read for a flag the record does not carry.
			NetH4TicketRecord ordinaryRecord = record;
			ordinaryRecord.persistentWorld = false;
			ordinaryRecord.recordVersion = NetReconnectTicketStore::RecordVersionFor(false);
			std::vector<uint8_t> ordinaryBytes;
			if (ordinaryRecord.recordVersion != NetReconnectTicketStore::c_DirectoryRecordVersion ||
			    !NetReconnectTicketStore::Serialize(ordinaryRecord, ordinaryBytes) || ordinaryBytes.size() + 1 != bytes.size()) {
				return Fail("ticket-rejoin-did-not-target-the-world: an ordinary ticket wrote version " +
				            std::to_string(ordinaryRecord.recordVersion) + " and " + std::to_string(ordinaryBytes.size()) +
				            " bytes against the world's " + std::to_string(bytes.size()));
			}
			NetH4TicketRecord ordinaryDecoded;
			if (!NetReconnectTicketStore::Deserialize(ordinaryBytes, ordinaryDecoded) || ordinaryDecoded.persistentWorld ||
			    ordinaryDecoded.directorySessionId != ordinaryRecord.directorySessionId) {
				return Fail("ticket-rejoin-did-not-target-the-world: an ordinary ticket read back world " +
				            std::string(ordinaryDecoded.persistentWorld ? "true" : "false") + " session \"" +
				            ordinaryDecoded.directorySessionId + "\"");
			}
			// A world record cannot be written into a body that has no flag byte to carry it.
			NetH4TicketRecord mislabelled = record;
			mislabelled.recordVersion = NetReconnectTicketStore::c_DirectoryRecordVersion;
			std::vector<uint8_t> refused;
			if (NetReconnectTicketStore::Serialize(mislabelled, refused)) {
				return Fail("ticket-rejoin-did-not-target-the-world: a world record serialized into a version " +
				            std::to_string(mislabelled.recordVersion) + " body that cannot carry its flag");
			}
			return 0;
		}

		int TestSpectatorActivationWaitsForItsImage() {
			std::string error;
			NetMatchConfig config = MakeWorldConfig();
			NetWorldJoinHost host;
			if (!host.Configure(config, MakeIdentity(), &error)) {
				return Fail("spectator host did not configure: " + error);
			}
			// One slot, so the second connection is an overflow spectator.
			if (!host.BeginJoin(7, 2, "alice", 1000, &error) || !host.BeginJoin(8, 3, "bob", 1000, &error)) {
				return Fail("spectator host did not open both joins: " + error);
			}
			const NetWorldJoinSession* spectator = host.FindSession(8);
			if (spectator == nullptr || !spectator->spectator) {
				return Fail("the overflow connection did not become a spectator");
			}
			uint64_t announced = 0;
			if (host.ScheduleSpectatorActivation(8, 100, &announced, &error)) {
				return Fail("spectator-activation-before-its-image: E " + std::to_string(announced) + " was announced with no transfer started");
			}
			if (host.FindSession(8)->phase != NetWorldJoinPhase::SnapshotTransfer) {
				return Fail("spectator-activation-before-its-image: the spectator left the phase its transfer is retried in");
			}
			NetWorldCheckpointImage image;
			image.worldId = c_WorldId;
			image.boot = 1;
			image.round = 1;
			image.tick = 40;
			image.bytes = 8;
			image.digest = "d";
			image.path = "Worlds/image.bin";
			host.PublishImage(image);
			if (!host.NoteTransferStarted(8, 0x22, 2, 0)) {
				return Fail("spectator transfer start refused");
			}
			if (!host.ScheduleSpectatorActivation(8, 100, &announced, &error) || announced != 100 + c_NetWorldActivationLeadFrames) {
				return Fail("spectator-activation-before-its-image: E was not announced once the image was on the way: " + error);
			}
			return 0;
		}

		int TestSpectatorNeverTakesASeat() {
			std::string error;
			NetWorldJoinHost host;
			if (!host.Configure(MakeWorldConfig(), MakeIdentity(), &error) || !host.BeginJoin(7, 2, "alice", 1000, &error) ||
			    !host.BeginJoin(8, 3, "bob", 1000, &error)) {
				return Fail("spectator host did not open both joins: " + error);
			}
			const NetWorldJoinSession* member = host.FindSession(7);
			const NetWorldJoinSession* spectator = host.FindSession(8);
			if (member == nullptr || spectator == nullptr || !spectator->spectator) {
				return Fail("the overflow connection did not become a spectator");
			}
			const NetWorldActivationPlan spectatorPlan = PlanWorldActivation(*spectator, 200, false);
			if (spectatorPlan.admit || spectatorPlan.submitTransition) {
				return Fail("spectator-activated-a-seat: admit " + std::string(spectatorPlan.admit ? "true" : "false") +
				            ", transition " + std::string(spectatorPlan.submitTransition ? "true" : "false"));
			}
			const NetWorldActivationPlan memberPlan = PlanWorldActivation(*member, 200, false);
			if (!memberPlan.admit || !memberPlan.submitTransition) {
				return Fail("a member's activation stopped admitting");
			}
			const NetGameWorldTransition transition = BuildWorldActivateTransition(*spectator, MakeWorldConfig(), host.Membership().Revision());
			if (transition.bindBrain || WorldTransitionBindsBrain(transition, true)) {
				return Fail("spectator-activated-a-seat: a spectator's transition binds a brain");
			}
			return 0;
		}

		int TestSpectatorLobbyIdsRecycle() {
			std::string error;
			NetWorldJoinHost host;
			if (!host.Configure(MakeWorldConfig(), MakeIdentity(), &error) || !host.BeginJoin(7, 2, "alice", 1000, &error)) {
				return Fail("spectator id host did not open the member join: " + error);
			}
			std::vector<NetPeerId> spectators;
			for (size_t i = 0; i < c_WorldSpectatorLobbyCap; ++i) {
				const NetPeerId connection = static_cast<NetPeerId>(100 + i);
				if (!host.BeginJoin(connection, static_cast<uint16_t>(10 + i), "watcher", 1000, &error)) {
					return Fail("a spectator join was refused: " + error);
				}
				spectators.push_back(connection);
			}
			const uint8_t first = host.FindSession(spectators.front())->spectatorLobbyPeer;
			const uint8_t second = host.FindSession(spectators[1])->spectatorLobbyPeer;
			if (first != c_WorldSpectatorLobbyPeerFirst || second != c_WorldSpectatorLobbyPeerFirst + 1 || first == second) {
				return Fail("two overflow spectators shared one lobby id: " + std::to_string(static_cast<int>(first)) + " and " +
				            std::to_string(static_cast<int>(second)));
			}
			// Past the cap there is no id to bind, which is a stream refused rather than one stolen.
			if (!host.BeginJoin(200, 200, "overflow", 1000, &error)) {
				return Fail("the spectator past the cap was refused a bootstrap: " + error);
			}
			if (host.FindSession(200)->spectatorLobbyPeer != 0) {
				return Fail("spectator-lobby-id-leaked: the spectator past the cap took id " +
				            std::to_string(static_cast<int>(host.FindSession(200)->spectatorLobbyPeer)));
			}
			host.CancelJoin(200, "past the cap");
			// A spectator that leaves returns its id to the pool: the cap is concurrent, not lifetime.
			host.CancelJoin(spectators[1], "left");
			if (!host.BeginJoin(300, 300, "later", 1000, &error)) {
				return Fail("a later spectator was refused: " + error);
			}
			if (host.FindSession(300)->spectatorLobbyPeer != second) {
				return Fail("spectator-lobby-id-leaked: the freed id " + std::to_string(static_cast<int>(second)) + " was not reused, the later spectator got " +
				            std::to_string(static_cast<int>(host.FindSession(300)->spectatorLobbyPeer)));
			}
			// A connection the session no longer lists ends its bootstrap, id and all.
			const std::vector<NetPeerId> live = {7, 300};
			if (host.ReleaseLostConnections(live) == 0 || host.FindSession(spectators.front()) != nullptr) {
				return Fail("spectator-lobby-id-leaked: a dropped spectator kept its bootstrap");
			}
			if (host.FindSession(7) == nullptr || host.FindSession(300) == nullptr) {
				return Fail("spectator-lobby-id-leaked: a live bootstrap was ended");
			}
			return 0;
		}

		NetIdentityManifest MakeSessionIdentity() {
			NetIdentityManifest manifest;
			manifest.gameVersion = "7.0.0-test";
			manifest.networkProtocolVersion = NetProtocol::c_Version;
			manifest.controllerFrameVersion = ControllerFrame::c_Version;
			manifest.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
			manifest.buildId = "stage2-world";
			manifest.platform = "test";
			manifest.deterministicConfigHash.fill(1);
			manifest.moduleManifestHash.fill(2);
			manifest.sessionRulesHash.fill(3);
			manifest.sessionIdentityHash.fill(4);
			manifest.hasUserdataModules = false;
			return manifest;
		}

		NetSessionConfig MakeWorldSessionConfig(uint16_t port, uint64_t nonce, const std::string& name) {
			NetSessionConfig config;
			config.localIdentity = MakeSessionIdentity();
			config.displayName = name;
			config.port = port;
			config.sessionId = 0x5700000000000000ULL + port;
			config.localNonce = nonce;
			config.maxPeers = 4;
			config.heartbeatIntervalMs = 50;
			config.timeoutMs = 4000;
			return config;
		}

		// A joiner activates inside a sim update: the start has to hand that update back and finish its
		// handshake over the updates that follow, whatever the remote does.
		int TestWorldJoinLockstepStartDoesNotHoldTheSimUpdate() {
			std::string error;
			const uint16_t port = 47119;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetSession session;
			NetSession clientSession;
			NetMatchRunner runner;
			NetLockstepCoordinator round;
			NetMatchRunnerConfig config;
			config.host = true;
			config.matchConfig = MakeWorldConfig();
			config.useLobbyProtocol = false;
			config.sessionConfig = MakeWorldSessionConfig(port, 0x11ULL, "World");
			config.sessionWaitMs = 2000;
			config.lobbyWaitMs = 2000;
			config.lockstepWaitMs = 1500;
			// The deadline is the joiner's own updates, so ANY start that blocks overruns it at once.
			config.worldJoinStartWaitTicks = 4;
			config.missingFrameGraceMs = 1000000;
			config.postSessionSettleMs = 0;
			config.postLobbySettleMs = 0;
			// A world with no one in it starts alone, so this runner is started without a second thread.
			if (!runner.Start(hostTransport, session, round, config, &error)) {
				return Fail("world lockstep start: the empty world host did not start: " + error);
			}
			NetSessionConfig clientConfig = MakeWorldSessionConfig(port, 0x22ULL, "Joiner");
			if (!clientSession.StartClient(clientTransport, "loopback", clientConfig, &error)) {
				return Fail("world lockstep start: the joining session did not start: " + error);
			}
			for (uint64_t now = 0; now <= 2000 && session.GetReadyPeers().empty(); now += 10) {
				session.Tick(now);
				clientSession.Tick(now);
				hostTransport.AdvanceTimeMs(10);
				clientTransport.AdvanceTimeMs(10);
			}
			if (session.GetReadyPeers().empty() || clientSession.GetReadyPeers().empty()) {
				return Fail("world lockstep start: the joining peer never reached Ready");
			}

			// The remote is silent from here: nothing answers the lockstep handshake until this case
			// starts the other side. The sessions are not ticked again, so only the round plane reads the wire.
			const uint64_t e = 240;
			NetLockstepCoordinator joinRound;
			if (runner.StartWorldJoinLockstep(hostTransport, session, joinRound, e, &error)) {
				return Fail("world-join-lockstep-held-the-sim-update: the start reported a running lockstep while the remote was silent");
			}
			if (!runner.IsWorldJoinLockstepStarting()) {
				return Fail("world-join-lockstep-held-the-sim-update: the start resolved inside 1 update (runner " +
				            std::string(NetMatchRunner::StateName(runner.GetState())) + ", error \"" + error +
				            "\") instead of handing the update back");
			}
			int updates = 1;
			for (; updates < static_cast<int>(config.worldJoinStartWaitTicks); ++updates) {
				if (runner.PumpWorldJoinLockstepStart(joinRound, &error)) {
					return Fail("world-join-lockstep-held-the-sim-update: the silent remote produced a running lockstep after " +
					            std::to_string(updates) + " updates");
				}
				if (!runner.IsWorldJoinLockstepStarting()) {
					return Fail("world-join-lockstep-held-the-sim-update: the start gave up after " + std::to_string(updates) +
					            " updates (" + error + "); a silent remote must cost updates, not one held update");
				}
			}

			// Released: the other end answers, and the start finishes over the updates that follow.
			NetLockstepCoordinator joinerSide;
			NetLockstepConfig joinerConfig;
			joinerConfig.sessionId = session.GetSessionId();
			joinerConfig.localPeerId = 2;
			joinerConfig.remotePeerId = 1;
			joinerConfig.remoteTransportPeerId = clientSession.GetReadyPeers().front().transportPeerId;
			joinerConfig.peerCount = config.matchConfig.peerCount;
			joinerConfig.timeoutMs = 1000000;
			joinerConfig.startFrame = e;
			joinerConfig.matchConfig = config.matchConfig;
			if (!joinerSide.Start(clientTransport, joinerConfig, &error)) {
				return Fail("world-join-lockstep-did-not-start: the answering side did not start: " + error);
			}
			int released = 0;
			for (uint64_t now = 3000; released < 400 && !joinRound.IsRunning(); ++released, now += 10) {
				(void)runner.PumpWorldJoinLockstepStart(joinRound, &error);
				joinerSide.Tick(now);
				hostTransport.AdvanceTimeMs(10);
				clientTransport.AdvanceTimeMs(10);
			}
			if (!joinRound.IsRunning() || runner.GetState() != NetMatchRuntimeState::Running) {
				return Fail("world-join-lockstep-did-not-start: after " + std::to_string(released) +
				            " updates with the remote answering the round is " + (joinRound.IsRunning() ? "running" : "not running") +
				            " and the runner is " + NetMatchRunner::StateName(runner.GetState()) + " (" + error + ")");
			}
			if (runner.IsWorldJoinLockstepStarting()) {
				return Fail("world-join-lockstep-did-not-start: the start never closed out after " + std::to_string(released) + " updates");
			}
			if (joinRound.GetConfig().startFrame != e) {
				return Fail("world-join-lockstep-did-not-start: the round started at frame " +
				            std::to_string(joinRound.GetConfig().startFrame) + ", E is " + std::to_string(e));
			}
			return 0;
		}

		int TestTypedAddressTargetsOnlyItsHost() {
			std::vector<NetDirectoryClient::GameRow> rows;
			NetDirectoryClient::GameRow world;
			world.source = "NET";
			world.name = "World";
			world.activity = "Persistent World";
			world.address = "198.51.100.7";
			world.port = 42124;
			world.persistentWorld = true;
			NetDirectoryClient::GameRow ordinary;
			ordinary.source = "LAN";
			ordinary.name = "Duel";
			ordinary.activity = "P4 Alpha Duel";
			ordinary.address = "203.0.113.9";
			ordinary.port = 8484;
			rows.push_back(world);
			rows.push_back(ordinary);
			std::string activity;
			// The world row is highlighted while another host's address is typed: the typed host decides.
			if (NetDirectoryClient::TargetsPersistentWorld(rows, 0, ordinary.address, ordinary.port, "", 0, &activity)) {
				return Fail("typed-address-targeted-the-wrong-world: a highlighted world row made " + ordinary.address + " a world target");
			}
			if (!activity.empty()) {
				return Fail("typed-address-targeted-the-wrong-world: the ordinary target took the activity \"" + activity + "\"");
			}
			if (!NetDirectoryClient::TargetsPersistentWorld(rows, 0, world.address, world.port, "", 0, &activity) || activity != world.activity) {
				return Fail("typed-address-targeted-the-wrong-world: the world's own address did not target it");
			}
			if (!NetDirectoryClient::TargetsPersistentWorld(rows, 1, world.address, world.port, "", 0, nullptr)) {
				return Fail("typed-address-targeted-the-wrong-world: a listed world at the typed address was missed");
			}
			// The last world this process joined still answers for an address no row covers.
			if (!NetDirectoryClient::TargetsPersistentWorld(rows, -1, "192.0.2.5", 7000, "192.0.2.5", 7000, nullptr)) {
				return Fail("typed-address-targeted-the-wrong-world: the last world target was forgotten");
			}
			if (NetDirectoryClient::TargetsPersistentWorld(rows, -1, "192.0.2.5", 7001, "192.0.2.5", 7000, nullptr)) {
				return Fail("typed-address-targeted-the-wrong-world: another port on the last world's host was taken for it");
			}
			return 0;
		}

		int TestSecondJoinAtTheSameTickGetsTheImage() {
			std::string error;
			NetWorldJoinHost host;
			if (!host.Configure(MakeWorldConfig(), MakeIdentity(), &error) || !host.BeginJoin(7, 2, "alice", 1000, &error)) {
				return Fail("second join host did not open the first join: " + error);
			}
			NetWorldCheckpointImage image;
			image.worldId = c_WorldId;
			image.boot = 1;
			image.round = 1;
			image.tick = 40;
			image.bytes = 8;
			image.digest = "d";
			image.path = "Worlds/image.bin";
			host.PublishImage(image);
			// The image is already frozen at this tick, so the next connection bootstraps from it.
			if (!host.BeginJoin(8, 3, "bob", 1100, &error)) {
				return Fail("the second join was refused: " + error);
			}
			if (host.FindSession(8)->snapshotTick != image.tick) {
				return Fail("second-join-had-no-image: snapshotTick " + std::to_string(host.FindSession(8)->snapshotTick) +
				            " while the published image stands at " + std::to_string(image.tick));
			}
			if (!host.NoteTransferComplete(8, image.bytes, &error)) {
				return Fail("second-join-had-no-image: its finished transfer was refused: " + error);
			}
			return 0;
		}

		int TestQueuedImageDoesNotReplaceTheOneInFlight() {
			std::string error;
			WorldLobbyPair pair;
			if (!pair.Open(47117, &error)) {
				return Fail("queue lobby pair: " + error);
			}
			// Big enough to span many StateChunks, so the second image is offered mid-stream.
			const std::vector<uint8_t> archive(512U * 1024U, 0x41);
			NetWorldCheckpointImage image;
			image.worldId = c_WorldId;
			image.boot = 1;
			image.round = 1;
			image.tick = 12;
			image.bytes = archive.size();
			image.digest = DigestWorldJoinBytes(archive);
			image.path = "Worlds/queue.bin";
			std::vector<uint8_t> first;
			std::vector<uint8_t> second;
			if (!EncodeWorldJoinImageBlob(image, archive, {}, first, &error) || !EncodeWorldJoinImageBlob(image, archive, {}, second, &error)) {
				return Fail("queue blob did not encode: " + error);
			}
			if (!pair.host.BindLateRemote(2, pair.hostRemote, &error) ||
			    !pair.host.BindLateRemote(c_WorldSpectatorLobbyPeerFirst, pair.hostRemote, &error)) {
				return Fail("queue bind: " + error);
			}
			if (!pair.host.BeginStateTransferTo(2, first)) {
				return Fail("the first joiner's image did not take the pump");
			}
			const uint64_t firstTransfer = pair.host.GetOutgoingStateId();
			pair.Pump(1);
			if (!pair.host.IsStateTransferOutgoing()) {
				return Fail("the first joiner's image finished before the second was offered");
			}
			if (pair.host.BeginStateTransferTo(c_WorldSpectatorLobbyPeerFirst, second)) {
				return Fail("joiner-image-replaced: the second image took the pump while the first was still streaming");
			}
			if (pair.host.QueuedStateTransfers() != 1) {
				return Fail("joiner-image-replaced: the second image was dropped instead of queued");
			}
			if (pair.host.GetOutgoingStateId() != firstTransfer) {
				return Fail("joiner-image-replaced: the outgoing transfer changed under the first joiner");
			}
			for (int i = 0; i < 80 && !pair.client.HasCompleteStateTransfer(); ++i) {
				pair.Pump(1);
			}
			if (!pair.client.HasCompleteStateTransfer() || pair.client.TakeReceivedState() != first) {
				return Fail("joiner-image-replaced: the first joiner did not receive its own image");
			}
			pair.Pump(2);
			if (pair.host.QueuedStateTransfers() != 0 || pair.host.GetOutgoingStateId() == firstTransfer) {
				return Fail("joiner-image-replaced: the queued image never took the free pump");
			}
			return 0;
		}

		int TestActivateBindingOnBothPeers() {
			std::string error;
			NetWorldJoinHost host;
			if (!host.Configure(MakeWorldConfig(), MakeIdentity(), &error) || !host.BeginJoin(7, 2, "alice", 1000, &error)) {
				return Fail("binding host did not open a join: " + error);
			}
			const uint64_t e = 120;
			NetWorldJoinSession session = *host.FindSession(7);
			session.activationTick = e;
			const NetGameWorldTransition transition = BuildWorldActivateTransition(session, MakeWorldConfig(), host.Membership().Revision());
			if (transition.className.empty() || transition.preset != "Brain Robot" || !transition.bindBrain ||
			    transition.player < 0 || transition.peerId != 2) {
				return Fail("activate-binding-missing: class \"" + transition.className + "\" preset \"" + transition.preset +
				            "\" binds " + std::string(transition.bindBrain ? "a brain" : "none") + " player " +
				            std::to_string(transition.player) + " peer " + std::to_string(static_cast<int>(transition.peerId)));
			}
			NetLockstepFrame frame;
			frame.senderPeerId = 1;
			frame.targetFrame = e;
			frame.roundId = 1;
			NetGameCommand command;
			command.senderPeerId = 1;
			command.payload = transition;
			frame.commands.push_back(command);
			std::vector<uint8_t> recovery;
			NetLockstepError encodeError;
			if (!NetLockstepCodec::EncodeRecoveryInput(frame, recovery, &encodeError)) {
				return Fail("activate-binding-missing: recovery encode failed: " + encodeError.message);
			}
			NetLockstepFrame hostView;
			if (!NetLockstepCodec::DecodeRecoveryInput(recovery, hostView, nullptr)) {
				return Fail("activate-binding-missing: the host could not decode its own committed activate");
			}
			// The joiner's view is not a copy of that buffer: it comes off the world tail, through the
			// lobby, out of the joiner's own catch-up step.
			WorldLobbyPair pair;
			if (!pair.Open(47118, &error)) {
				return Fail("activate-binding-missing: lobby pair: " + error);
			}
			NetWorldCheckpointImage image;
			image.worldId = c_WorldId;
			image.boot = 1;
			image.round = 1;
			image.tick = e - 1;
			image.bytes = 8;
			image.digest = "d";
			image.path = "Worlds/binding.bin";
			host.PublishImage(image);
			if (!host.NoteTransferComplete(7, image.bytes, &error)) {
				return Fail("activate-binding-missing: the bootstrap could not finish its transfer: " + error);
			}
			if (!host.Tail().Append(frame, &error)) {
				return Fail("activate-binding-missing: the committed tail refused the activate frame: " + error);
			}
			if (!pair.host.BindLateRemote(2, pair.hostRemote, &error)) {
				return Fail("activate-binding-missing: bind: " + error);
			}
			NetMatchService::SendWorldJoinTailTo(pair.host, host, *host.FindSession(7));
			pair.Pump(8);
			NetWorldCatchUpClient catchUp;
			catchUp.active = true;
			catchUp.snapshotTick = e - 1;
			catchUp.appliedThrough = e - 1;
			if (!ScenarioRunner::InstallWorldCatchUp(e - 1, {}, &error)) {
				return Fail("activate-binding-missing: catch-up install: " + error);
			}
			ScenarioRunner::SetWorldCatchUpActivation(e + 1);
			NetMatchService::StepWorldJoinCatchUpClient(pair.client, catchUp);
			NetLockstepReadyFrame applied;
			if (!ScenarioRunner::TakeWorldCatchUpReadyFrame(e, applied, &error)) {
				return Fail("activate-binding-missing: the joiner never received the activate frame at E: " + error);
			}
			const auto* hostApplied = hostView.commands.empty() ? nullptr : std::get_if<NetGameWorldTransition>(&hostView.commands[0].payload);
			const auto* joinerApplied = applied.remoteCommands.empty() ? nullptr : std::get_if<NetGameWorldTransition>(&applied.remoteCommands[0].payload);
			if (hostApplied == nullptr || joinerApplied == nullptr) {
				return Fail("activate-binding-missing: a peer's view of E carries no world transition");
			}
			if (hostApplied->activationFrame != e || joinerApplied->activationFrame != e) {
				return Fail("activate-binding-missing: the peers disagree on E: host " + std::to_string(hostApplied->activationFrame) +
				            ", joiner " + std::to_string(joinerApplied->activationFrame));
			}
			if (!WorldTransitionBindsBrain(*hostApplied, true)) {
				return Fail("activate-binding-missing: the host's apply binds no brain, player " + std::to_string(hostApplied->player));
			}
			if (!WorldTransitionBindsBrain(*joinerApplied, true)) {
				return Fail("activate-binding-missing: the joiner's apply binds no brain, player " + std::to_string(joinerApplied->player));
			}
			if (WorldTransitionBindsBrain(*joinerApplied, false)) {
				return Fail("activate-binding-missing: a peer with no seated resident still bound a brain");
			}
			if (!ScenarioRunner::AcceptWorldTransition(*hostApplied, &error) ||
			    !ScenarioRunner::AcceptWorldTransition(*joinerApplied, &error)) {
				return Fail("activate-binding-missing: " + error);
			}

			LoopbackTransport hostTransport;
			LoopbackTransport joinerTransport;
			if (!hostTransport.StartHost(47115, &error) || !joinerTransport.Connect("loopback", 47115, &error)) {
				return Fail("activate-binding-missing: loopback pair: " + error);
			}
			NetLockstepCoordinator hostCoord;
			NetLockstepConfig hostConfig;
			hostConfig.sessionId = 4;
			hostConfig.localPeerId = 1;
			hostConfig.peerCount = 2;
			hostConfig.timeoutMs = 1000000;
			hostConfig.relayToOtherPeers = true;
			hostConfig.matchConfig = MakeWorldConfig();
			if (!hostCoord.Start(hostTransport, hostConfig, &error)) {
				return Fail("activate-binding-missing: host coordinator: " + error);
			}
			if (!hostCoord.AdmitWorldMember(hostApplied->peerId, 1, e, &error) || !hostCoord.IsWorldMember(hostApplied->peerId)) {
				return Fail("activate-binding-missing: the world did not admit peer " +
				            std::to_string(static_cast<int>(hostApplied->peerId)) + " at E " + std::to_string(e) + ": " + error);
			}
			NetLockstepCoordinator joinerCoord;
			NetLockstepConfig joinerConfig;
			joinerConfig.sessionId = 4;
			joinerConfig.localPeerId = joinerApplied->peerId;
			joinerConfig.remotePeerId = 1;
			joinerConfig.remoteTransportPeerId = 1;
			joinerConfig.peerCount = 2;
			joinerConfig.timeoutMs = 1000000;
			joinerConfig.matchConfig = MakeWorldConfig();
			if (!joinerCoord.Start(joinerTransport, joinerConfig, &error)) {
				return Fail("activate-binding-missing: joiner coordinator: " + error);
			}
			if (!joinerCoord.IsWorldMember(hostApplied->peerId) && joinerCoord.GetConfig().localPeerId != hostApplied->peerId) {
				return Fail("activate-binding-missing: the joiner's round runs as peer " +
				            std::to_string(static_cast<int>(joinerCoord.GetConfig().localPeerId)) + ", the host admitted peer " +
				            std::to_string(static_cast<int>(hostApplied->peerId)));
			}
			return 0;
		}

		int TestSlowJoinerReannounceThenFree() {
			std::string error;
			NetWorldJoinHost host;
			if (!host.Configure(MakeWorldConfig(), MakeIdentity(), &error) || !host.BeginJoin(7, 2, "alice", 1000, &error)) {
				return Fail("slow join did not open: " + error);
			}
			NetWorldCheckpointImage image;
			image.worldId = c_WorldId;
			image.boot = 1;
			image.round = 1;
			image.tick = 10;
			image.bytes = 4;
			image.digest = "d";
			image.path = "Worlds/image.bin";
			host.PublishImage(image);
			if (!host.NoteTransferComplete(7, 4, &error)) {
				return Fail(error);
			}
			uint64_t firstE = 0;
			if (!host.NoteCatchUpProgress(7, 10, 1, 1, 20, &firstE, &error) || firstE == 0) {
				return Fail("slow join did not announce E: " + error);
			}
			if (host.SlowActivation(firstE) != nullptr || host.SlowActivation(firstE + 1) == nullptr) {
				return Fail("slow joiner was not detected after E");
			}
			uint64_t later = 0;
			if (!host.ReannounceActivation(7, firstE + 1, &later, &error) || later <= firstE) {
				return Fail("slow joiner did not get one later E: " + error);
			}
			if (host.ReannounceActivation(7, later + 1, nullptr, &error)) {
				return Fail("slow joiner was re-announced more than once");
			}
			const uint32_t generation = host.Membership().Slots().front().generation;
			host.CancelJoin(7, "the joiner missed the announced activation");
			if (host.FindSession(7) != nullptr || host.Membership().Slots().front().generation == generation ||
			    host.Membership().HeldSlots() != 0) {
				return Fail("slow-joiner-did-not-free-slot");
			}
			return 0;
		}

	int TestH4CleanLeaveKeepsWorldSeatOpen() {
		ScriptedAuthCrypto crypto;
		ScopedTestCrypto scope(&crypto);
		const NetH4Identity identity = MakeH4Identity();
		const std::vector<NetH4Seat> seats = {{0, 0, 0, false, 1, true}, {1, 1, 1, false, 2, false}};
		auto commitJoin = [&](NetReconnectHost& host, NetPeerId connection, const char* name, NetH4TicketOffer& offer) -> bool {
			NetH4NewJoin join;
			join.identity = identity;
			join.displayName = name;
			join.txId.fill(static_cast<uint8_t>(connection));
			host.HandleMessage(connection, join, 0);
			bool found = false;
			for (const NetH4Outbound& outbound: host.TakeOutbound()) {
				if (const auto* ticket = std::get_if<NetH4TicketOffer>(&outbound.payload)) {
					offer = *ticket;
					found = true;
				}
			}
			if (!found) {
				return false;
			}
			host.HandleMessage(connection, NetH4TicketStoredAck{c_NetH4Version, offer.txId, offer.stableSeat, offer.holderGeneration, true}, 5);
			host.TakeOutbound();
			return true;
		};
		{
			NetSeatAuthRegistry registry;
			if (!registry.BeginHostedSession()) {
				return Fail("world H4 leave registry did not arm");
			}
			NetReconnectHost host;
			host.Configure(&registry, 0x5743ULL, identity);
			host.SetSeatTable(seats, NetMatchMode::PvPSkirmish);
			host.SetLiveMatch(true);
			host.SetPersistentWorld(true);
			NetH4TicketOffer offer;
			if (!commitJoin(host, 12, "alice", offer)) {
				return Fail("world H4 join did not offer a ticket");
			}
			NetH4LeaveRequest leave;
			leave.txId.fill(21);
			leave.epoch = offer.epoch;
			leave.stableSeat = offer.stableSeat;
			leave.holderGeneration = offer.holderGeneration;
			host.HandleMessage(12, leave, 10);
			bool closed = true;
			for (const NetH4SeatStatus& status: host.GetSeatStatuses()) {
				if (status.stableSeat == offer.stableSeat) {
					closed = status.closed;
				}
			}
			if (closed) {
				return Fail("H4 clean leave closed a persistent-world seat");
			}
		}
		{
			NetSeatAuthRegistry registry;
			if (!registry.BeginHostedSession()) {
				return Fail("ordinary H4 leave registry did not arm");
			}
			NetReconnectHost host;
			host.Configure(&registry, 0x5744ULL, identity);
			host.SetSeatTable(seats, NetMatchMode::PvPSkirmish);
			host.SetPersistentWorld(false);
			NetH4TicketOffer offer;
			if (!commitJoin(host, 13, "alice", offer)) {
				return Fail("ordinary H4 join did not offer a ticket");
			}
			host.SetLiveMatch(true);
			NetH4LeaveRequest leave;
			leave.txId.fill(22);
			leave.epoch = offer.epoch;
			leave.stableSeat = offer.stableSeat;
			leave.holderGeneration = offer.holderGeneration;
			host.HandleMessage(13, leave, 10);
			bool closed = false;
			for (const NetH4SeatStatus& status: host.GetSeatStatuses()) {
				if (status.stableSeat == offer.stableSeat) {
					closed = status.closed;
				}
			}
			if (!closed) {
				return Fail("H4 clean leave left an ordinary match seat open");
			}
		}
		return 0;
	}

	int TestH4LeaveThenNewJoinSameHolder() {
		ScriptedAuthCrypto crypto;
		ScopedTestCrypto scope(&crypto);
		const NetH4Identity identity = MakeH4Identity();
		const std::vector<NetH4Seat> seats = {{0, 0, 0, false, 1, true}, {1, 1, 1, false, 2, false}};
		NetSeatAuthRegistry registry;
		if (!registry.BeginHostedSession()) {
			return Fail("rejoin registry did not arm");
		}
		NetReconnectHost host;
		host.Configure(&registry, 0x5745ULL, identity);
		host.SetSeatTable(seats, NetMatchMode::PvPSkirmish);
		host.SetLiveMatch(true);
		host.SetPersistentWorld(true);
		NetH4NewJoin join;
		join.identity = identity;
		join.displayName = "alice";
		join.txId.fill(12);
		host.HandleMessage(12, join, 0);
		NetH4TicketOffer offer;
		bool offered = false;
		for (const NetH4Outbound& outbound: host.TakeOutbound()) {
			if (const auto* ticket = std::get_if<NetH4TicketOffer>(&outbound.payload)) {
				offer = *ticket;
				offered = true;
			}
		}
		if (!offered) {
			return Fail("rejoin after a clean leave did not land in the running world");
		}
		host.HandleMessage(12, NetH4TicketStoredAck{c_NetH4Version, offer.txId, offer.stableSeat, offer.holderGeneration, true}, 5);
		host.TakeOutbound();
		NetH4LeaveRequest leave;
		leave.txId.fill(21);
		leave.epoch = offer.epoch;
		leave.stableSeat = offer.stableSeat;
		leave.holderGeneration = offer.holderGeneration;
		host.HandleMessage(12, leave, 10);
		host.TakeOutbound();
		NetH4NewJoin rejoin = join;
		rejoin.txId.fill(14);
		host.HandleMessage(14, rejoin, 20);
		host.Tick(NetReconnectAdmission::c_DenialReleaseMs);
		bool landed = false;
		for (const NetH4Outbound& outbound: host.TakeOutbound()) {
			if (std::holds_alternative<NetH4TicketOffer>(outbound.payload)) {
				landed = true;
			}
			if (const auto* rejected = std::get_if<NetJoinRejected>(&outbound.payload)) {
				if (rejected->humanMessage == "the match is already in progress") {
					return Fail("rejoin after a clean leave did not land in the running world");
				}
			}
		}
		if (!landed) {
			return Fail("rejoin after a clean leave did not land in the running world");
		}
		return 0;
	}

	int TestDirectoryRowResumeAndWorldBootBounds() {
		std::string reason;
		nlohmann::json body = {
			{"name", "World"},
			{"activity", "Persistent World"},
			{"scene", "Grasslands"},
			{"mode", "pvp-skirmish"},
			{"peer_count", 2},
			{"seats_free", 1},
			{"game_version", "7.0.0"},
			{"build_id", "stage2-world"},
			{"network_protocol_version", 1},
			{"lockstep_codec_version", 22},
			{"controller_frame_version", 7},
			{"match_config_hash", std::string(64, 'a')},
			{"session_identity_hash", std::string(64, 'b')},
			{"module_manifest_hash", std::string(64, 'c')},
			{"listen_port", 42124},
			{"listen_addrs", nlohmann::json::array({"127.0.0.1"})},
			{"join_mode", "ip"},
			{"persistent_world", true},
			{"world_id", c_WorldId},
			{"world_boot", 0},
			{"resume_session_id", c_WorldId},
		};
		NetDirectoryRegisterRequest decoded;
		if (NetDirectoryCodec::DecodeRegisterRequest(body.dump(), decoded, reason)) {
			return Fail("world_boot 0 was accepted on the C++ register decoder");
		}
		body["world_boot"] = 1;
		if (!NetDirectoryCodec::DecodeRegisterRequest(body.dump(), decoded, reason) || decoded.worldBoot != 1 ||
		    decoded.resumeSessionId != c_WorldId) {
			return Fail("world_boot 1 / resume_session_id did not decode: " + reason);
		}
		body["world_boot"] = 1000000000;
		if (!NetDirectoryCodec::DecodeRegisterRequest(body.dump(), decoded, reason) || decoded.worldBoot != 1000000000) {
			return Fail("world_boot 10^9 did not decode: " + reason);
		}
		body["world_boot"] = 1000000001;
		if (NetDirectoryCodec::DecodeRegisterRequest(body.dump(), decoded, reason)) {
			return Fail("world_boot above 10^9 was accepted");
		}
		// The resume proof rides the same row: a reboot presents the token its last register issued.
		body["world_boot"] = 2;
		body["resume_token"] = "row-token-value";
		if (!NetDirectoryCodec::DecodeRegisterRequest(body.dump(), decoded, reason) || decoded.resumeToken != "row-token-value") {
			return Fail("world_boot 0 was accepted on the C++ register decoder: resume_token decoded as \"" +
			            decoded.resumeToken + "\", the row carries \"row-token-value\" (" + reason + ")");
		}
		const std::string encoded = NetDirectoryCodec::EncodeRegisterRequest(decoded);
		NetDirectoryRegisterRequest roundTrip;
		if (!NetDirectoryCodec::DecodeRegisterRequest(encoded, roundTrip, reason) || roundTrip.resumeToken != decoded.resumeToken ||
		    roundTrip.resumeSessionId != decoded.resumeSessionId) {
			return Fail("world_boot 0 was accepted on the C++ register decoder: the encoder wrote resume \"" +
			            roundTrip.resumeSessionId + "\"/\"" + roundTrip.resumeToken + "\" (" + reason + ")");
		}
		return 0;
	}

	int TestWorldTransitionCodec() {
		std::string error;
		NetWorldJoinHost host;
		if (!host.Configure(MakeWorldConfig(), MakeIdentity(), &error) || !host.BeginJoin(7, 2, "alice", 1000, &error)) {
			return Fail("codec host did not open a join: " + error);
		}
		const NetGameWorldTransition transition = BuildWorldActivateTransition(*host.FindSession(7), MakeWorldConfig(), host.Membership().Revision());
		NetLockstepPacket worldPacket;
		NetLockstepFrame worldFrame;
		worldFrame.senderPeerId = 1;
		worldFrame.targetFrame = 40;
		worldFrame.roundId = 1;
		NetGameCommand command;
		command.senderPeerId = 1;
		command.payload = transition;
		worldFrame.commands.push_back(command);
		worldPacket.payload = worldFrame;
		std::vector<uint8_t> worldBytes;
		NetLockstepError encodeError;
		if (!NetLockstepCodec::Encode(worldPacket, worldBytes, &encodeError) || worldBytes.size() < 6) {
			return Fail("WorldTransition frame did not encode: " + encodeError.message);
		}
		const uint16_t worldVersion = static_cast<uint16_t>(worldBytes[4] | (worldBytes[5] << 8));
		if (worldVersion != NetLockstepCodec::c_WorldTransitionVersion) {
			return Fail("WorldTransition frame did not stamp lockstep version 23");
		}
		const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(worldBytes);
		if (!decoded.ok) {
			return Fail("WorldTransition frame did not decode: " + decoded.error.message);
		}
		const auto* decodedFrame = std::get_if<NetLockstepFrame>(&decoded.packet.payload);
		if (decodedFrame == nullptr || decodedFrame->commands.size() != 1) {
			return Fail("WorldTransition decode lost the command");
		}
		const auto* roundTrip = std::get_if<NetGameWorldTransition>(&decodedFrame->commands[0].payload);
		if (roundTrip == nullptr || *roundTrip != transition || roundTrip->schema != c_NetWorldJoinSchema) {
			return Fail("WorldTransition codec did not round-trip");
		}
		std::vector<uint8_t> recovery;
		if (!NetLockstepCodec::EncodeRecoveryInput(worldFrame, recovery, &encodeError) ||
		    !NetLockstepCodec::DecodeRecoveryInput(recovery, worldFrame, &encodeError)) {
			return Fail("WorldTransition recovery did not round-trip at the world lockstep version: " + encodeError.message);
		}
		return 0;
	}

	int TestOrdinaryIdentityStamps() {
		std::string error;
		NetLockstepPacket ordinaryPacket;
		NetLockstepFrame ordinaryFrame;
		ordinaryFrame.senderPeerId = 1;
		ordinaryFrame.targetFrame = 1;
		ordinaryFrame.roundId = 1;
		ordinaryPacket.payload = ordinaryFrame;
		std::vector<uint8_t> ordinaryBytes;
		NetLockstepError encodeError;
		if (!NetLockstepCodec::Encode(ordinaryPacket, ordinaryBytes, &encodeError) || ordinaryBytes.size() < 6) {
			return Fail("ordinary frame did not encode: " + encodeError.message);
		}
		const uint16_t ordinaryVersion = static_cast<uint16_t>(ordinaryBytes[4] | (ordinaryBytes[5] << 8));
		if (ordinaryVersion != NetLockstepCodec::c_Version || ordinaryVersion != 22) {
			return Fail("ordinary lockstep frame did not stamp version 22");
		}
		NetIdentityManifest manifest;
		NetIdentityBuildOptions options;
		if (!NetIdentity::BuildCurrentManifest(manifest, &error, options)) {
			return Fail("ordinary identity manifest did not build: " + error);
		}
		if (manifest.deterministicConfig.lockstepCodecVersion != NetLockstepCodec::c_Version ||
		    manifest.deterministicConfig.matchConfigVersion != NetMatchConfigUtil::c_Version) {
			return Fail("ordinary identity did not stamp lockstep 22 and match config 4");
		}
		NetIdentity::StampOptionsForTarget(options, true);
		if (!NetIdentity::BuildCurrentManifest(manifest, &error, options) ||
		    manifest.deterministicConfig.lockstepCodecVersion != NetLockstepCodec::c_WorldTransitionVersion ||
		    manifest.deterministicConfig.matchConfigVersion != NetMatchConfigUtil::c_PersistentWorldVersion) {
			return Fail("world identity did not stamp the live world versions");
		}
		return 0;
	}

	int TestDueActivationAdmits() {
		LoopbackTransport transport;
		std::string error;
		if (!transport.StartHost(47114, &error)) {
			return Fail("loopback host: " + error);
		}
		NetLockstepCoordinator world;
		NetLockstepConfig worldConfig;
		worldConfig.sessionId = 3;
		worldConfig.localPeerId = 1;
		worldConfig.peerCount = 2;
		worldConfig.timeoutMs = 1000000;
		worldConfig.relayToOtherPeers = true;
		worldConfig.matchConfig = MakeWorldConfig();
		if (!world.Start(transport, worldConfig, &error)) {
			return Fail("admit world start: " + error);
		}
		NetWorldJoinHost host;
		if (!host.Configure(MakeWorldConfig(), MakeIdentity(), &error) || !host.BeginJoin(7, 2, "alice", 1000, &error)) {
			return Fail("admit join did not open: " + error);
		}
		NetWorldCheckpointImage image;
		image.worldId = c_WorldId;
		image.boot = 1;
		image.round = 1;
		image.tick = 1;
		image.bytes = 4;
		image.digest = "d";
		image.path = "Worlds/image.bin";
		host.PublishImage(image);
		if (!host.NoteTransferComplete(7, 4, &error)) {
			return Fail(error);
		}
		const uint64_t nowFrame = world.GetStats().nextFrame;
		uint64_t activation = 0;
		if (!host.NoteCatchUpProgress(7, nowFrame, 1, 1, nowFrame, &activation, &error) || activation != nowFrame + c_NetWorldActivationLeadFrames) {
			return Fail("admit catch-up did not announce E ahead of nextFrame: " + error);
		}
		if (!host.NoteCatchUpProgress(7, activation - 1, 1, 1, nowFrame, nullptr, &error)) {
			return Fail("admit catch-up refused appliedThrough E-1: " + error);
		}
		if (host.DueActivation(activation - 1) == nullptr) {
			return Fail("a due activation at E-1 was not ready");
		}
		if (host.DueActivation(activation) != nullptr) {
			return Fail("a pump at nextFrame >= E still treated the member as due");
		}
		if (host.LateActivation(activation) == nullptr) {
			return Fail("a late Admit path was not offered after E-1");
		}
		if (activation <= nowFrame) {
			return Fail("announced E was not ahead of nextFrame");
		}
		if (!world.AdmitWorldMember(2, 1, activation, &error)) {
			return Fail("a due activation cancelled instead of admitting: " + error);
		}
		// The announce chooses E ahead of everything the round has already put on the wire, so a member
		// is a peer of the round before the frames it owes go out.
		NetWorldJoinHost ahead;
		if (!ahead.Configure(MakeWorldConfig(), MakeIdentity(), &error) || !ahead.BeginJoin(9, 3, "bob", 1000, &error)) {
			return Fail("admit announce host did not open a join: " + error);
		}
		ahead.PublishImage(image);
        if (!ahead.NoteTransferComplete(9, 4, &error)) {
			return Fail("admit announce transfer: " + error);
		}
		const uint64_t sentThrough = 500;
		ahead.NoteSentInputThrough(sentThrough);
		uint64_t announced = 0;
		if (!ahead.NoteCatchUpProgress(9, sentThrough, 1, 1, sentThrough - 1, &announced, &error) || announced == 0) {
			return Fail("admit announce did not announce E: " + error);
		}
		if (announced <= sentThrough) {
			return Fail("world-member-admitted-behind-the-sent-input: the announce handed out E " + std::to_string(announced) +
			            " with input already sent through " + std::to_string(sentThrough));
		}

		// A world already running with a member in it, its dictionary live: the admission of a second
		// member may not move anything under the first, and what the replay leaves the new member's
		// table holding has to be what the live table holds.
		NetMatchConfig worldRunning = MakeWorldConfig();
		worldRunning.peerCount = 3;
		worldRunning.inputDelayFrames = 4;
		worldRunning.players = {
		    NetMatchPlayerSlot{1, 0, true, "World"},
		    NetMatchPlayerSlot{2, 1, false, "Joiner"},
		    NetMatchPlayerSlot{3, 2, false, "Resident"},
		};
		LoopbackTransport hostLink;
		LoopbackTransport memberLink;
		LoopbackTransport residentLink;
		if (!hostLink.StartHost(47124, &error) || !memberLink.Connect("loopback", 47124, &error) ||
		    !residentLink.Connect("loopback", 47124, &error)) {
			return Fail("admit replay loopback: " + error);
		}
		std::vector<NetPeerId> hostRemotes;
		for (const NetTransportEvent& event: hostLink.PollEvents()) {
			if (event.type == NetTransportEventType::PeerConnected) hostRemotes.push_back(event.peerId);
		}
		NetPeerId memberRemote = c_InvalidNetPeerId;
		for (const NetTransportEvent& event: memberLink.PollEvents()) {
			if (event.type == NetTransportEventType::PeerConnected) memberRemote = event.peerId;
		}
		NetPeerId residentRemote = c_InvalidNetPeerId;
		for (const NetTransportEvent& event: residentLink.PollEvents()) {
			if (event.type == NetTransportEventType::PeerConnected) residentRemote = event.peerId;
		}
		if (hostRemotes.size() != 2 || memberRemote == c_InvalidNetPeerId || residentRemote == c_InvalidNetPeerId) {
			return Fail("admit replay loopback: the three peers never connected");
		}
		NetLockstepCoordinator running;
		NetLockstepConfig runningConfig;
		runningConfig.sessionId = 21;
		runningConfig.localPeerId = 1;
		runningConfig.peerCount = 3;
		runningConfig.timeoutMs = 1000000;
		runningConfig.relayToOtherPeers = true;
		runningConfig.inputDelayFrames = 4;
		runningConfig.frameRedundancyTicks = 4;
		runningConfig.matchConfig = worldRunning;
		if (!running.Start(hostLink, runningConfig, &error)) {
			return Fail("admit replay world start: " + error);
		}
		const uint64_t first = running.GetStats().nextFrame;
		// The resident joined before any of the input this case is about.
		if (!running.AdmitWorldMember(3, hostRemotes[1], first + 1, &error)) {
			return Fail("admit replay resident admission: " + error);
		}
		NetLockstepCoordinator resident;
		NetLockstepConfig residentConfig;
		residentConfig.sessionId = 21;
		residentConfig.localPeerId = 3;
		residentConfig.remoteTransportPeerIds[1] = residentRemote;
		residentConfig.peerCount = 3;
		residentConfig.timeoutMs = 1000000;
		residentConfig.inputDelayFrames = 4;
		residentConfig.frameRedundancyTicks = 4;
		residentConfig.startFrame = first + 1;
		residentConfig.joinsRunningRound = true;
		residentConfig.matchConfig = worldRunning;
		if (!resident.Start(residentLink, residentConfig, &error)) {
			return Fail("existing-member-lost-the-dictionary-across-an-admission: peer 3 did not start: " + error);
		}
		for (uint64_t now = 0; now < 400 && !resident.IsRunning(); now += 10) {
			running.Tick(now);
			resident.Tick(now);
			hostLink.AdvanceTimeMs(10);
			residentLink.AdvanceTimeMs(10);
		}
		if (!resident.IsRunning()) {
			return Fail("existing-member-lost-the-dictionary-across-an-admission: peer 3 is " +
			            std::string(resident.IsFailed() ? "failed: " + resident.GetStats().timeoutReason : "not running"));
		}
		// One reading before the epoch, so the resident's table for the host is not empty when it moves.
		NetSoundObservation early;
		early.objectUID = 7001;
		early.tick = first + 1;
		early.phase = 1;
		early.occurrence = 1;
		early.ordinal = 0;
		early.value = 0.5F;
		for (uint64_t produced = 1; produced <= 3; ++produced) {
			std::vector<NetSoundObservation> observations;
			if (produced == 1) observations.push_back(early);
			if (!running.QueueLocalInput(produced, {}, {}, &error, observations)) {
				return Fail("admit replay pre-epoch queue at " + std::to_string(produced) + ": " + error);
			}
		}
		// The announce: E is ahead of everything sent, and it is where every sender starts over.
		const uint64_t e = running.SentInputThrough() + 1;
		running.SetObservationEpoch(e);
		if (running.ObservationEpoch() != e) {
			return Fail("world-member-missed-the-frames-sent-before-its-admission: the announce set the epoch at " +
			            std::to_string(running.ObservationEpoch()) + ", E is " + std::to_string(e));
		}
		NetSoundObservation reading;
		reading.objectUID = 4242;
		reading.tick = e + 1;
		reading.phase = 1;
		reading.occurrence = 1;
		reading.ordinal = 0;
		reading.value = 0.25F;
		for (uint64_t produced = 4; produced <= 7; ++produced) {
			std::vector<NetSoundObservation> observations;
			if (produced + runningConfig.inputDelayFrames == e + 1) observations.push_back(reading);
			if (!running.QueueLocalInput(produced, {}, {}, &error, observations)) {
				return Fail("admit replay queue at " + std::to_string(produced) + ": " + error);
			}
		}
		// The resident owes its own frames from its start plus its delay; without them it cannot commit
		// the frame the reading rides.
		for (uint64_t produced = 1; produced <= 8; ++produced) {
			(void)resident.QueueLocalInput(produced, {}, {}, &error);
		}
		if (running.SentInputThrough() != e + 3) {
			return Fail("world-member-missed-the-frames-sent-before-its-admission: the world sent through " +
			            std::to_string(running.SentInputThrough()) + ", E..E+3 is " + std::to_string(e) + ".." + std::to_string(e + 3));
		}
		if (!running.AdmitWorldMember(2, hostRemotes[0], e, &error)) {
			return Fail("world-member-missed-the-frames-sent-before-its-admission: the admission at E " +
			            std::to_string(e) + " was refused: " + error);
		}
		if (running.LastAdmissionReplayFrames() < 4) {
			return Fail("world-member-missed-the-frames-sent-before-its-admission: the admission replayed " +
			            std::to_string(running.LastAdmissionReplayFrames()) + " frames for targets " +
			            std::to_string(e) + ".." + std::to_string(running.SentInputThrough()));
		}
		NetLockstepCoordinator member;
		NetLockstepConfig memberConfig;
		memberConfig.sessionId = 21;
		memberConfig.localPeerId = 2;
		memberConfig.remoteTransportPeerIds[1] = memberRemote;
		memberConfig.peerCount = 3;
		memberConfig.timeoutMs = 1000000;
		memberConfig.inputDelayFrames = 4;
		memberConfig.frameRedundancyTicks = 4;
		memberConfig.startFrame = e;
		memberConfig.joinsRunningRound = true;
		memberConfig.matchConfig = worldRunning;
		if (!member.Start(memberLink, memberConfig, &error)) {
			return Fail("world-member-missed-the-frames-sent-before-its-admission: the member's round did not start: " + error);
		}
		// The host keeps producing, window and all: one more reading on the key the epoch already bound,
		// which the live table spells as a slot. The member can only read it if the replay left its
		// table where the live one is.
		if (!running.QueueLocalInput(8, {}, {}, &error, {reading})) {
			return Fail("replayed-member-table-diverged-from-the-live-table: the live frame after the replay did not queue: " + error);
		}
		(void)member.QueueLocalInput(8, {}, {}, &error);
		for (uint64_t now = 400; now < 1600; now += 10) {
			running.Tick(now);
			member.Tick(now);
			resident.Tick(now);
			hostLink.AdvanceTimeMs(10);
			memberLink.AdvanceTimeMs(10);
			residentLink.AdvanceTimeMs(10);
		}
		if (member.IsFailed()) {
			return Fail("window-copy-before-the-members-start-failed-the-round: " + member.GetStats().timeoutReason +
			            " after " + std::to_string(member.GetStats().windowCopiesSkipped) + " skipped copies");
		}
		auto readingsOf = [&](NetLockstepCoordinator& peer, std::vector<uint64_t>& frames, uint64_t& sawReadingAt, uint64_t& sawEarlyAt) {
			for (NetLockstepReadyFrame ready; peer.PopReadyFrame(ready);) {
				frames.push_back(ready.frame);
				for (const NetSoundObservation& observed: ready.remoteObservations) {
					if (observed.objectUID == reading.objectUID && observed.tick == reading.tick) sawReadingAt = ready.frame;
					if (observed.objectUID == early.objectUID && observed.tick == early.tick) sawEarlyAt = ready.frame;
				}
			}
		};
		std::vector<uint64_t> residentFrames;
		uint64_t residentReadingAt = 0;
		uint64_t residentEarlyAt = 0;
		readingsOf(resident, residentFrames, residentReadingAt, residentEarlyAt);
		if (resident.IsFailed() || residentReadingAt != e + 1 || residentEarlyAt != first + 1) {
			return Fail("existing-member-lost-the-dictionary-across-an-admission: peer 3 " +
			            std::string(resident.IsFailed() ? "failed: " + resident.GetStats().timeoutReason : "is running") +
			            ", read the pre-epoch key at " + std::to_string(residentEarlyAt) + " and the epoch's at " +
			            std::to_string(residentReadingAt) + ", E+1 is " + std::to_string(e + 1));
		}
		std::vector<uint64_t> memberFrames;
		uint64_t memberReadingAt = 0;
		uint64_t memberEarlyAt = 0;
		readingsOf(member, memberFrames, memberReadingAt, memberEarlyAt);
		std::string heldText;
		for (const uint64_t frame: memberFrames) heldText += (heldText.empty() ? "" : ",") + std::to_string(frame);
		if (memberFrames.size() < 4 || memberFrames.front() != e) {
			return Fail("world-member-missed-the-frames-sent-before-its-admission: joiner holds frames " + heldText +
			            ", needs " + std::to_string(e) + ".." + std::to_string(e + 3));
		}
		if (memberReadingAt != e + 1) {
			return Fail("world-member-cannot-decode-the-observation-dictionary: the member committed " + heldText +
			            " and read the world's reading at " + std::to_string(memberReadingAt) + ", it rides " + std::to_string(e + 1));
		}
		// The live frame past the replay: the host spelled its key as a slot, so the member's table must
		// be the live table's.
		if (std::find(memberFrames.begin(), memberFrames.end(), e + 4) == memberFrames.end()) {
			return Fail("replayed-member-table-diverged-from-the-live-table: the member holds " + heldText +
			            " and never committed the live frame " + std::to_string(e + 4));
		}
		return 0;
	}

	int TestStaleWorldTransitionRefused() {
		NetGameWorldTransition live;
		live.schema = c_NetWorldJoinSchema;
		live.kind = NetGameWorldTransition::Activate;
		live.peerId = 7;
		live.holderGeneration = 4;
		live.membershipRevision = 9;
		live.team = 0;
		if (!ScenarioRunner::AcceptWorldTransition(live, nullptr)) {
			return Fail("a current world transition was refused");
		}
		NetGameWorldTransition staleGeneration = live;
		staleGeneration.holderGeneration = 3;
		if (ScenarioRunner::AcceptWorldTransition(staleGeneration, nullptr)) {
			return Fail("a stale holder generation was applied");
		}
		NetGameWorldTransition staleRevision = live;
		staleRevision.holderGeneration = 4;
		staleRevision.membershipRevision = 8;
		if (ScenarioRunner::AcceptWorldTransition(staleRevision, nullptr)) {
			return Fail("a stale membership revision was applied");
		}
		// The schema fence: the only version gate an applied world command has.
		for (const uint16_t schema: {uint16_t{0}, static_cast<uint16_t>(c_NetWorldJoinSchema + 1)}) {
			NetGameWorldTransition offSchema = live;
			offSchema.schema = schema;
			offSchema.holderGeneration = 5;
			offSchema.membershipRevision = 10;
			if (ScenarioRunner::AcceptWorldTransition(offSchema, nullptr)) {
				return Fail("a stale holder generation was applied: schema " + std::to_string(schema) +
				            " was accepted, the live schema is " + std::to_string(c_NetWorldJoinSchema));
			}
		}
		// An ordinary round decodes these packets now, so it must refuse to apply one.
		std::string error;
		LoopbackTransport ordinaryTransport;
		if (!ordinaryTransport.StartHost(47122, &error)) {
			return Fail("ordinary-round-applied-a-world-transition: loopback: " + error);
		}
		NetLockstepCoordinator ordinaryRound;
		NetLockstepConfig ordinaryConfig;
		ordinaryConfig.sessionId = 11;
		ordinaryConfig.localPeerId = 1;
		ordinaryConfig.peerCount = 2;
		ordinaryConfig.timeoutMs = 1000000;
		ordinaryConfig.matchConfig = NetMatchConfigUtil::MakeDefault(0x4F52440ULL);
		ordinaryConfig.matchConfig.players = {
		    NetMatchPlayerSlot{1, 0, false, "Host"},
		    NetMatchPlayerSlot{2, 1, false, "Client"},
		};
		if (!ordinaryRound.Start(ordinaryTransport, ordinaryConfig, &error)) {
			return Fail("ordinary-round-applied-a-world-transition: ordinary coordinator: " + error);
		}
		ScenarioRunner::SetLockstepCoordinator(&ordinaryRound);
		const bool acceptedOffPlane = ScenarioRunner::AcceptWorldTransition(live, nullptr);
		const bool worldRound = ScenarioRunner::IsPersistentWorld();
		ScenarioRunner::SetLockstepCoordinator(nullptr);
		if (acceptedOffPlane || worldRound) {
			return Fail("ordinary-round-applied-a-world-transition: persistent world " +
			            std::string(worldRound ? "true" : "false") + ", the host-stamped transition was " +
			            std::string(acceptedOffPlane ? "applied" : "refused"));
		}
		return 0;
	}

	int TestReadyFramePackIncludesRemotes() {
		NetLockstepReadyFrame ready;
		ready.frame = 11;
		ControllerFrame local;
		local.actorUniqueID = 1;
		ControllerFrame remote;
		remote.actorUniqueID = 2;
		ready.localFrames.push_back(local);
		ready.remoteFrames.push_back(remote);
		NetGameCommand command;
		command.senderPeerId = 2;
		ready.remoteCommands.push_back(command);
		const NetLockstepFrame packed = PackWorldJoinReadyFrame(ready);
		if (packed.targetFrame != 11 || packed.frames.size() != 2 || packed.commands.size() != 1) {
			return Fail("committed ready-frame pack dropped a remote Controller or command");
		}
		return 0;
	}

	// The world's configured capacity: the v5 block's own fields, the hash domain and the slot table.
	int TestWorldCapacityRidesTheV5Config() {
		const auto roundTrip = [](const NetMatchConfig& config, NetMatchConfig& out, std::vector<uint8_t>& wire) {
			wire.clear();
			if (!NetLobbyProtocol::Encode({NetLobbyMatchConfig{config}}, wire)) {
				return false;
			}
			const auto decoded = NetLobbyProtocol::Decode(wire);
			const NetLobbyMatchConfig* payload = decoded.ok ? std::get_if<NetLobbyMatchConfig>(&decoded.message.payload) : nullptr;
			if (payload == nullptr) {
				return false;
			}
			out = payload->config;
			return true;
		};
		NetMatchConfig world = MakeWorldConfig();
		world.peerCount = 3;
		world.players = {NetMatchPlayerSlot{1, 0, true, "World"}, NetMatchPlayerSlot{2, 0, false, "A"}, NetMatchPlayerSlot{3, 1, false, "B"}};
		world.worldTeamCapacity = {1, 1, 0, 0};
		world.worldMaxSpectators = 3;
		world.worldRespawnDelaySeconds = 20;
		NetMatchConfig back;
		std::vector<uint8_t> worldWire;
		if (!roundTrip(world, back, worldWire)) {
			return Fail("world-capacity-did-not-ride-the-v5-config: a v5 world config did not survive the lobby codec");
		}
		if (back.worldTeamCapacity != world.worldTeamCapacity || back.worldMaxSpectators != 3 || back.worldRespawnDelaySeconds != 20) {
			return Fail("world-capacity-did-not-ride-the-v5-config: read back capacity " +
			            std::to_string(static_cast<int>(back.worldTeamCapacity[0])) + "," + std::to_string(static_cast<int>(back.worldTeamCapacity[1])) +
			            " spectators " + std::to_string(static_cast<int>(back.worldMaxSpectators)) +
			            " respawn " + std::to_string(back.worldRespawnDelaySeconds));
		}
		// An ordinary config must not carry a byte of the world block, whatever the struct holds.
		NetMatchConfig ordinary = NetMatchConfigUtil::MakeDefault(0x4F52443ULL);
		std::vector<uint8_t> plainWire;
		NetMatchConfig plainBack;
		if (!roundTrip(ordinary, plainBack, plainWire)) {
			return Fail("world-capacity-did-not-ride-the-v5-config: an ordinary v4 config did not survive the lobby codec");
		}
		NetMatchConfig stuffed = ordinary;
		stuffed.worldTeamCapacity = {4, 4, 4, 4};
		stuffed.worldMaxSpectators = 9;
		stuffed.worldRespawnDelaySeconds = 99;
		std::vector<uint8_t> stuffedWire;
		NetMatchConfig stuffedBack;
		if (!roundTrip(stuffed, stuffedBack, stuffedWire)) {
			return Fail("world-capacity-did-not-ride-the-v5-config: a v4 config carrying capacity did not encode");
		}
		if (stuffedWire != plainWire) {
			return Fail("world-capacity-moved-an-ordinary-config: the v4 bytes grew from " + std::to_string(plainWire.size()) +
			            " to " + std::to_string(stuffedWire.size()) + " when the capacity fields were set");
		}
		if (NetMatchConfigUtil::HashConfig(stuffed) != NetMatchConfigUtil::HashConfig(ordinary)) {
			return Fail("world-capacity-moved-an-ordinary-config: the v4 hash changed when the capacity fields were set");
		}
		// The world's own hash reads every capacity field, so a joiner validates the contract it was offered.
		NetMatchConfig narrower = world;
		narrower.worldTeamCapacity = {2, 0, 0, 0};
		NetMatchConfig fewerWatchers = world;
		fewerWatchers.worldMaxSpectators = 1;
		NetMatchConfig slowerRespawn = world;
		slowerRespawn.worldRespawnDelaySeconds = 21;
		if (NetMatchConfigUtil::HashConfig(narrower) == NetMatchConfigUtil::HashConfig(world) ||
		    NetMatchConfigUtil::HashConfig(fewerWatchers) == NetMatchConfigUtil::HashConfig(world) ||
		    NetMatchConfigUtil::HashConfig(slowerRespawn) == NetMatchConfigUtil::HashConfig(world)) {
			return Fail("world-capacity-left-the-v5-hash: two worlds with different capacity hash alike");
		}
		// The spectator byte is found by diffing two encodings, so no literal offset rides this case.
		NetMatchConfig moved = world;
		moved.worldMaxSpectators = 4;
		std::vector<uint8_t> movedWire;
		NetMatchConfig movedBack;
		if (!roundTrip(moved, movedBack, movedWire) || movedWire.size() != worldWire.size()) {
			return Fail("world-capacity-did-not-ride-the-v5-config: the spectator bound changed the encoded length");
		}
		size_t spectatorOffset = movedWire.size();
		size_t differences = 0;
		for (size_t index = 0; index < movedWire.size(); ++index) {
			if (movedWire[index] != worldWire[index]) {
				spectatorOffset = index;
				++differences;
			}
		}
		if (differences != 1) {
			return Fail("world-capacity-did-not-ride-the-v5-config: the spectator bound moved " + std::to_string(differences) + " bytes");
		}
		std::vector<uint8_t> overBound = worldWire;
		overBound[spectatorOffset] = static_cast<uint8_t>(NetMatchConfigUtil::c_MaxWorldSpectators + 1);
		if (NetLobbyProtocol::Decode(overBound).ok) {
			return Fail("world-capacity-decoded-past-its-bound: the codec accepted " +
			            std::to_string(NetMatchConfigUtil::c_MaxWorldSpectators + 1) + " spectators");
		}
		// The validator is the host's fence: a capacity may never promise more seats than the roster has.
		std::string reason;
		NetMatchConfig tooMany = world;
		tooMany.worldTeamCapacity = {2, 1, 0, 0};
		if (NetMatchConfigUtil::ValidateLocalAlpha(tooMany, &reason) || reason != "world team capacity exceeds the roster's human slots") {
			return Fail("world-capacity-passed-validation: an over-capacity world was accepted with reason \"" + reason + "\"");
		}
		NetMatchConfig tooManyWatchers = world;
		tooManyWatchers.worldMaxSpectators = static_cast<uint8_t>(NetMatchConfigUtil::c_MaxWorldSpectators + 1);
		if (NetMatchConfigUtil::ValidateLocalAlpha(tooManyWatchers, &reason) || reason != "world_max_spectators is out of range") {
			return Fail("world-capacity-passed-validation: an over-bound spectator count was accepted with reason \"" + reason + "\"");
		}
		NetMatchConfig slowWorld = world;
		slowWorld.worldRespawnDelaySeconds = static_cast<uint16_t>(NetMatchConfigUtil::c_MaxWorldRespawnDelaySeconds + 1);
		if (NetMatchConfigUtil::ValidateLocalAlpha(slowWorld, &reason) || reason != "world_respawn_delay_seconds is out of range") {
			return Fail("world-capacity-passed-validation: an over-bound respawn delay was accepted with reason \"" + reason + "\"");
		}
		NetMatchConfig legacy = ordinary;
		legacy.worldMaxSpectators = 1;
		if (NetMatchConfigUtil::ValidateLocalAlpha(legacy, &reason) || reason != "an ordinary match cannot carry world capacity") {
			return Fail("world-capacity-passed-validation: an ordinary match kept a world capacity with reason \"" + reason + "\"");
		}
		// The slot table is the capacity, spent in team order, so every peer names the same team for the same id.
		NetWorldMembership membership;
		NetMatchConfig coop = MakeWorldConfig();
		coop.peerCount = 3;
		coop.players = {NetMatchPlayerSlot{1, 0, true, "World"}, NetMatchPlayerSlot{2, 0, false, "A"}, NetMatchPlayerSlot{3, 0, false, "B"}};
		coop.worldTeamCapacity = {2, 0, 0, 0};
		if (!membership.Configure(coop, &reason) || membership.Slots().size() != 2) {
			return Fail("world-capacity-left-the-slot-table: a capacity of two built " +
			            std::to_string(membership.Slots().size()) + " slots (" + reason + ")");
		}
		if (membership.Slots()[0].peerId != 2 || membership.Slots()[0].team != 0 ||
		    membership.Slots()[1].peerId != 3 || membership.Slots()[1].team != 0) {
			return Fail("world-capacity-left-the-slot-table: slots are peer " + std::to_string(static_cast<int>(membership.Slots()[0].peerId)) +
			            " team " + std::to_string(static_cast<int>(membership.Slots()[0].team)) + " and peer " +
			            std::to_string(static_cast<int>(membership.Slots()[1].peerId)) + " team " +
			            std::to_string(static_cast<int>(membership.Slots()[1].team)));
		}
		NetMatchConfig split = coop;
		split.worldTeamCapacity = {1, 1, 0, 0};
		if (!membership.Configure(split, &reason) || membership.Slots().size() != 2 ||
		    membership.Slots()[0].team != 0 || membership.Slots()[1].team != 1) {
			return Fail("world-capacity-left-the-slot-table: a split capacity did not put peer 3 on team 1 (" + reason + ")");
		}
		NetMatchConfig oneSeat = coop;
		oneSeat.worldTeamCapacity = {1, 0, 0, 0};
		if (!membership.Configure(oneSeat, &reason) || membership.Slots().size() != 1) {
			return Fail("world-capacity-left-the-slot-table: a capacity of one built " + std::to_string(membership.Slots().size()) + " slots");
		}
		// The record a boot reads back names the same world, so a restart offers the same seats.
		NetWorldIdentity identity = MakeIdentity();
		identity.teamCapacity = {1, 2, 0, 0};
		identity.maxSpectators = 5;
		identity.respawnDelaySeconds = 30;
		NetWorldIdentity reread;
		if (!NetWorldIdentityFile::Decode(NetWorldIdentityFile::Encode(identity), reread, &reason) || reread != identity) {
			return Fail("world-capacity-left-the-identity-record: the record read back capacity " +
			            std::to_string(static_cast<int>(reread.teamCapacity[0])) + "," + std::to_string(static_cast<int>(reread.teamCapacity[1])) +
			            " spectators " + std::to_string(static_cast<int>(reread.maxSpectators)) + " (" + reason + ")");
		}
		NetWorldIdentity bare = MakeIdentity();
		if (!NetWorldIdentityFile::Decode(NetWorldIdentityFile::Encode(bare), reread, &reason) || reread != bare ||
		    WorldIdentityCarriesCapacity(reread)) {
			return Fail("world-capacity-left-the-identity-record: a record with no capacity did not read back bare (" + reason + ")");
		}
		return 0;
	}

	// Overflow spectators are bounded: past the host's bound the world refuses before any transfer.
	int TestSpectatorOverflowIsBounded() {
		NetMatchConfig config = MakeWorldConfig();
		config.peerCount = 3;
		config.players = {NetMatchPlayerSlot{1, 0, true, "World"}, NetMatchPlayerSlot{2, 0, false, "A"}, NetMatchPlayerSlot{3, 1, false, "B"}};
		config.worldTeamCapacity = {1, 1, 0, 0};
		config.worldMaxSpectators = 1;
		NetWorldJoinHost host;
		std::string error;
		if (!host.Configure(config, MakeIdentity(), &error)) {
			return Fail("world-full-refusal-missing: the world plane refused its own config (" + error + ")");
		}
		if (host.SpectatorBound() != 1 || host.SpectatorsFree() != 1) {
			return Fail("world-spectator-bound-ignored: the bound reads " + std::to_string(host.SpectatorBound()) +
			            " with " + std::to_string(host.SpectatorsFree()) + " free");
		}
		// Two seats fill first; neither is a spectator and neither spends a watcher slot.
		if (!host.BeginJoin(11, 11, "a", 1000, &error) || !host.BeginJoin(12, 12, "b", 1000, &error)) {
			return Fail("world-seats-refused-below-capacity: a configured seat was refused (" + error + ")");
		}
		const NetWorldJoinSession* first = host.FindSession(11);
		const NetWorldJoinSession* second = host.FindSession(12);
		if (first == nullptr || second == nullptr || first->spectator || second->spectator ||
		    first->assignedPeerId != 2 || second->assignedPeerId != 3) {
			return Fail("world-seats-refused-below-capacity: the first two joiners took peers " +
			            std::to_string(first == nullptr ? 0 : first->assignedPeerId) + " and " +
			            std::to_string(second == nullptr ? 0 : second->assignedPeerId));
		}
		if (host.SpectatorsFree() != 1) {
			return Fail("world-spectator-bound-ignored: seating two members left " + std::to_string(host.SpectatorsFree()) + " watcher slots");
		}
		// The third joiner watches: no slot, no Controller, but a lobby id of its own.
		if (!host.BeginJoin(13, 13, "c", 1000, &error)) {
			return Fail("world-full-refusal-missing: the first overflow joiner was refused (" + error + ")");
		}
		const NetWorldJoinSession* watcher = host.FindSession(13);
		if (watcher == nullptr || !watcher->spectator || watcher->assignedPeerId != 0 ||
		    watcher->spectatorLobbyPeer < c_WorldSpectatorLobbyPeerFirst) {
			return Fail("world-full-refusal-missing: the overflow joiner is spectator " +
			            std::to_string(watcher != nullptr && watcher->spectator) + " on lobby id " +
			            std::to_string(watcher == nullptr ? 0 : watcher->spectatorLobbyPeer));
		}
		if (host.SpectatorCount() != 1 || host.SpectatorsFree() != 0) {
			return Fail("world-spectator-bound-ignored: one watcher left " + std::to_string(host.SpectatorsFree()) + " free");
		}
		// The fourth is turned away before any image work, with the reason the joiner shows.
		error.clear();
		if (host.BeginJoin(14, 14, "d", 1000, &error)) {
			return Fail("world-admitted-past-its-bound: a fourth joiner was admitted with the bound spent");
		}
		if (error != "the world is full") {
			return Fail("world-full-refusal-missing: the refusal reads \"" + error + "\"");
		}
		if (host.FindSession(14) != nullptr) {
			return Fail("world-admitted-past-its-bound: the refused joiner still holds a bootstrap");
		}
		// The refusal is answered once and remembered until the connection goes.
		if (!host.NoteRefusal(14, NetWorldJoinRefusal::WorldFull) || host.NoteRefusal(14, NetWorldJoinRefusal::WorldFull)) {
			return Fail("world-full-refusal-missing: the refusal was not recorded once");
		}
		if (host.RefusalOf(14) != NetWorldJoinRefusal::WorldFull || host.RefusalOf(13) != NetWorldJoinRefusal::None) {
			return Fail("world-full-refusal-missing: the refusal did not stay on connection 14 alone");
		}
		host.ReleaseLostConnections({11, 12, 13});
		if (host.RefusalOf(14) != NetWorldJoinRefusal::None) {
			return Fail("world-full-refusal-missing: a gone connection kept its refusal");
		}
		// A watcher that leaves frees its slot for the next one.
		host.ReleaseLostConnections({11, 12});
		if (host.SpectatorCount() != 0 || host.SpectatorsFree() != 1) {
			return Fail("world-spectator-bound-ignored: a departed watcher left " + std::to_string(host.SpectatorsFree()) + " free");
		}
		// A bound of zero admits no watcher at all; a bound wider than the id pool is held to the pool.
		NetMatchConfig closed = config;
		closed.worldMaxSpectators = 0;
		NetWorldJoinHost noWatchers;
		if (!noWatchers.Configure(closed, MakeIdentity(), &error) || noWatchers.SpectatorBound() != 0) {
			return Fail("world-spectator-bound-ignored: a zero bound reads " + std::to_string(noWatchers.SpectatorBound()));
		}
		if (!noWatchers.BeginJoin(21, 21, "a", 1000, &error) || !noWatchers.BeginJoin(22, 22, "b", 1000, &error)) {
			return Fail("world-seats-refused-below-capacity: a seat was refused with watchers closed (" + error + ")");
		}
		error.clear();
		if (noWatchers.BeginJoin(23, 23, "c", 1000, &error) || error != "the world is full") {
			return Fail("world-admitted-past-its-bound: a closed world admitted a watcher with reason \"" + error + "\"");
		}
		NetMatchConfig wide = config;
		wide.worldMaxSpectators = NetMatchConfigUtil::c_MaxWorldSpectators;
		NetWorldJoinHost widest;
		if (!widest.Configure(wide, MakeIdentity(), &error) || widest.SpectatorBound() != c_WorldSpectatorLobbyCap) {
			return Fail("world-spectator-bound-ignored: the widest bound reads " + std::to_string(widest.SpectatorBound()) +
			            " against a pool of " + std::to_string(c_WorldSpectatorLobbyCap));
		}
		// The refusal rides the same 9-byte report every other world answer does.
		const NetLobbyStateChunk chunk = MakeWorldJoinReport(c_NetWorldReportRefused, static_cast<uint64_t>(NetWorldJoinRefusal::WorldFull));
		uint8_t kind = 0;
		uint64_t value = 0;
		if (!ParseWorldJoinReport(chunk, kind, value) || kind != c_NetWorldReportRefused ||
		    value != static_cast<uint64_t>(NetWorldJoinRefusal::WorldFull) ||
		    std::string(NetWorldJoinRefusalText(value)) != "the world is full") {
			return Fail("world-full-refusal-missing: the refusal report parsed as kind " + std::to_string(static_cast<int>(kind)) +
			            " value " + std::to_string(value));
		}
		// The directory row carries the count a browser needs to tell "full" from "closed".
		NetDirectoryRegisterRequest row;
		row.name = "world";
		row.activity = "Persistent World";
		row.scene = "Grasslands";
		row.mode = "coop-pve";
		row.joinMode = "ip";
		row.listenPort = 8123;
		row.listenAddrs = {"127.0.0.1"};
		row.persistentWorld = true;
		row.worldId = c_WorldId;
		row.worldBoot = 1;
		row.seatsFree = 0;
		row.spectatorFree = 4;
		NetDirectoryRegisterRequest back;
		std::string reason;
		if (!NetDirectoryCodec::DecodeRegisterRequest(NetDirectoryCodec::EncodeRegisterRequest(row), back, reason) ||
		    back.spectatorFree != 4 || back.seatsFree != 0) {
			return Fail("world-row-lost-its-spectator-count: the row read back seats " + std::to_string(back.seatsFree) +
			            " watchers " + std::to_string(back.spectatorFree) + " (" + reason + ")");
		}
		NetDirectoryRegisterRequest ordinary = row;
		ordinary.persistentWorld = false;
		ordinary.worldId.clear();
		ordinary.worldBoot = 0;
		if (NetDirectoryCodec::EncodeRegisterRequest(ordinary).find("spectator_free") != std::string::npos) {
			return Fail("world-row-lost-its-spectator-count: an ordinary row carried spectator_free");
		}
		NetDirectoryHeartbeatRequest beat;
		beat.token = "t";
		beat.peerCount = 3;
		beat.seatsFree = 0;
		beat.spectatorFree = 2;
		NetDirectoryHeartbeatRequest beatBack;
		if (!NetDirectoryCodec::DecodeHeartbeatRequest(NetDirectoryCodec::EncodeHeartbeatRequest(beat), beatBack, reason) ||
		    !beatBack.spectatorFree.has_value() || *beatBack.spectatorFree != 2) {
			return Fail("world-row-lost-its-spectator-count: a beat lost its watcher count (" + reason + ")");
		}
		NetDirectoryHeartbeatRequest silent = beat;
		silent.spectatorFree.reset();
		if (!NetDirectoryCodec::DecodeHeartbeatRequest(NetDirectoryCodec::EncodeHeartbeatRequest(silent), beatBack, reason) ||
		    beatBack.spectatorFree.has_value()) {
			return Fail("world-row-lost-its-spectator-count: a beat that named no watcher count carried one");
		}
		return 0;
	}

	// Two bootstraps in flight: one image, one capture, one admission at a time, both restarts kept.
	int TestConcurrentJoinsKeepTheirOwnActivation() {
		NetMatchConfig config = MakeWorldConfig();
		config.peerCount = 3;
		config.players = {NetMatchPlayerSlot{1, 0, true, "World"}, NetMatchPlayerSlot{2, 0, false, "A"}, NetMatchPlayerSlot{3, 1, false, "B"}};
		config.worldTeamCapacity = {1, 1, 0, 0};
		config.worldMaxSpectators = 2;
		NetWorldJoinHost host;
		std::string error;
		if (!host.Configure(config, MakeIdentity(), &error)) {
			return Fail("concurrent-joins-shared-an-activation: the world plane refused its own config (" + error + ")");
		}
		// The first joiner opens before any image; the second opens after it is published.
		if (!host.BeginJoin(31, 31, "first", 1000, &error)) {
			return Fail("concurrent-joins-shared-an-activation: the first bootstrap was refused (" + error + ")");
		}
		NetWorldCheckpointImage image;
		image.worldId = c_WorldId;
		image.boot = 1;
		image.round = 1;
		image.tick = 500;
		image.bytes = 64;
		image.digest = "d";
		host.PublishImage(image);
		if (!host.BeginJoin(32, 32, "second", 1010, &error)) {
			return Fail("concurrent-joins-shared-an-activation: the second bootstrap was refused (" + error + ")");
		}
		const NetWorldJoinSession* first = host.FindSession(31);
		const NetWorldJoinSession* second = host.FindSession(32);
		if (first == nullptr || second == nullptr || first->assignedPeerId != 2 || second->assignedPeerId != 3) {
			return Fail("concurrent-joins-shared-an-activation: the two joiners took peers " +
			            std::to_string(first == nullptr ? 0 : first->assignedPeerId) + " and " +
			            std::to_string(second == nullptr ? 0 : second->assignedPeerId));
		}
		if (first->snapshotTick != 500 || second->snapshotTick != 500) {
			return Fail("concurrent-joins-recaptured-the-world: the two bootstraps hold ticks " +
			            std::to_string(first->snapshotTick) + " and " + std::to_string(second->snapshotTick) +
			            " from one published image at 500");
		}
		// One image, one capture: publishing it again at the same tick must not count a second stall.
		const uint64_t captures = host.Metrics().Captures();
		if (captures != 1) {
			return Fail("concurrent-joins-recaptured-the-world: two bootstraps at one tick cost " +
			            std::to_string(captures) + " captures");
		}
		// Each joiner is announced its own E, in join order, both ahead of the input already sent.
		host.NoteSentInputThrough(560);
		(void)host.NoteTransferComplete(31, 64, &error);
		(void)host.NoteTransferComplete(32, 64, &error);
		uint64_t firstE = 0;
		uint64_t secondE = 0;
		if (!host.NoteCatchUpProgress(31, 540, 40, 10, 560, &firstE, &error) || firstE == 0) {
			return Fail("concurrent-joins-shared-an-activation: the first joiner was announced no E (" + error + ")");
		}
		host.NoteSentInputThrough(600);
		if (!host.NoteCatchUpProgress(32, 580, 80, 20, 600, &secondE, &error) || secondE == 0) {
			return Fail("concurrent-joins-shared-an-activation: the second joiner was announced no E (" + error + ")");
		}
		if (firstE == secondE || secondE < firstE) {
			return Fail("concurrent-joins-shared-an-activation: the joiners were announced E " +
			            std::to_string(firstE) + " and " + std::to_string(secondE) + " out of join order");
		}
		if (firstE <= 560 || secondE <= 600) {
			return Fail("concurrent-joins-shared-an-activation: an E landed behind the input already sent (" +
			            std::to_string(firstE) + ", " + std::to_string(secondE) + ")");
		}
		// The due activation is the earlier one, so the members activate in join order.
		const NetWorldJoinSession* due = host.DueActivation(firstE - 1);
		if (due == nullptr || due->connection != 31) {
			return Fail("concurrent-joins-shared-an-activation: the activation due at " + std::to_string(firstE) +
			            " is connection " + std::to_string(due == nullptr ? 0 : due->connection));
		}
		if (host.DueActivation(secondE - 1) == nullptr || host.DueActivation(secondE - 1)->connection != 32) {
			return Fail("concurrent-joins-shared-an-activation: the second activation never came due at " + std::to_string(secondE));
		}
		// The round keeps BOTH restarts: a second announcement may not drop the first joiner's.
		NetLockstepCoordinator round;
		round.SetObservationEpoch(firstE);
		round.SetObservationEpoch(secondE);
		if (round.ObservationEpochs().size() != 2 || round.ObservationEpochs().count(firstE) == 0 ||
		    round.ObservationEpochs().count(secondE) == 0) {
			return Fail("concurrent-join-dropped-the-first-restart: the round holds " +
			            std::to_string(round.ObservationEpochs().size()) + " restarts, not " +
			            std::to_string(firstE) + " and " + std::to_string(secondE));
		}
		if (round.ObservationEpoch() != secondE) {
			return Fail("concurrent-join-dropped-the-first-restart: the newest restart reads " +
			            std::to_string(round.ObservationEpoch()) + ", not " + std::to_string(secondE));
		}
		// A re-announce moves its own restart and leaves the other joiner's standing.
		round.MoveObservationEpoch(secondE, secondE + 90);
		if (round.ObservationEpochs().count(firstE) == 0 || round.ObservationEpochs().count(secondE) != 0 ||
		    round.ObservationEpochs().count(secondE + 90) == 0) {
			return Fail("concurrent-join-dropped-the-first-restart: a re-announce left " +
			            std::to_string(round.ObservationEpochs().size()) + " restarts and lost one of them");
		}
		return 0;
	}

	int RunNamed(const char* name) {
		if (std::strcmp(name, "identity") == 0 || std::strcmp(name, "-net-world-identity-selftest") == 0) {
			s_FailTag = "net-world-identity-selftest";
			return TestIdentitySurvivesRestart();
		}
		if (std::strcmp(name, "identity-print") == 0 || std::strcmp(name, "-net-world-identity-print-selftest") == 0) {
			s_FailTag = "net-world-identity-print-selftest";
			return TestIdentityPrintOnce();
		}
		if (std::strcmp(name, "live") == 0 || std::strcmp(name, "-net-world-live-selftest") == 0) {
			s_FailTag = "net-world-live-selftest";
			return TestWorldLiveJoinAdmission();
		}
		if (std::strcmp(name, "ordinary-live") == 0 || std::strcmp(name, "-net-world-ordinary-live-selftest") == 0) {
			s_FailTag = "net-world-ordinary-live-selftest";
			return TestOrdinaryLiveJoinAdmission();
		}
		if (std::strcmp(name, "leave") == 0 || std::strcmp(name, "-net-world-leave-selftest") == 0) {
			s_FailTag = "net-world-leave-selftest";
			return TestWorldStartsEmptyAndSurvivesLastLeave();
		}
		if (std::strcmp(name, "rejoin") == 0 || std::strcmp(name, "-net-world-rejoin-selftest") == 0) {
			s_FailTag = "net-world-rejoin-selftest";
			return TestH4LeaveThenNewJoinSameHolder();
		}
		if (std::strcmp(name, "transfer") == 0 || std::strcmp(name, "-net-world-transfer-selftest") == 0) {
			s_FailTag = "net-world-transfer-selftest";
			return TestJoinerTransferPump();
		}
		if (std::strcmp(name, "digest") == 0 || std::strcmp(name, "-net-world-digest-selftest") == 0) {
			s_FailTag = "net-world-digest-selftest";
			return TestImageBlobRoundTrip();
		}
		if (std::strcmp(name, "catchup") == 0 || std::strcmp(name, "-net-world-catchup-selftest") == 0) {
			s_FailTag = "net-world-catchup-selftest";
			return TestCatchUpJoinerReport();
		}
		if (std::strcmp(name, "binding") == 0 || std::strcmp(name, "-net-world-binding-selftest") == 0) {
			s_FailTag = "net-world-binding-selftest";
			return TestActivateBindingOnBothPeers();
		}
		if (std::strcmp(name, "catchup-gap") == 0 || std::strcmp(name, "-net-world-catchup-gap-selftest") == 0) {
			s_FailTag = "net-world-catchup-gap-selftest";
			return TestCatchUpStopsAtTailGap();
		}
		if (std::strcmp(name, "catchup-ceiling") == 0 || std::strcmp(name, "-net-world-catchup-ceiling-selftest") == 0) {
			s_FailTag = "net-world-catchup-ceiling-selftest";
			return TestCatchUpKeepsItsCeiling();
		}
		if (std::strcmp(name, "take-pump") == 0 || std::strcmp(name, "-net-world-take-pump-selftest") == 0) {
			s_FailTag = "net-world-take-pump-selftest";
			return TestTakeDoesNotPumpTheSession();
		}
		if (std::strcmp(name, "watermark") == 0 || std::strcmp(name, "-net-world-watermark-selftest") == 0) {
			s_FailTag = "net-world-watermark-selftest";
			return TestImageWatermarkSurvivesAnEmptyCopy();
		}
		if (std::strcmp(name, "ticket") == 0 || std::strcmp(name, "-net-world-ticket-selftest") == 0) {
			s_FailTag = "net-world-ticket-selftest";
			return TestTicketRejoinTargetsTheWorld();
		}
		if (std::strcmp(name, "spectator-image") == 0 || std::strcmp(name, "-net-world-spectator-image-selftest") == 0) {
			s_FailTag = "net-world-spectator-image-selftest";
			return TestSpectatorActivationWaitsForItsImage();
		}
		if (std::strcmp(name, "spectator-seat") == 0 || std::strcmp(name, "-net-world-spectator-seat-selftest") == 0) {
			s_FailTag = "net-world-spectator-seat-selftest";
			return TestSpectatorNeverTakesASeat();
		}
		if (std::strcmp(name, "spectator-ids") == 0 || std::strcmp(name, "-net-world-spectator-ids-selftest") == 0) {
			s_FailTag = "net-world-spectator-ids-selftest";
			return TestSpectatorLobbyIdsRecycle();
		}
		if (std::strcmp(name, "lockstep-start") == 0 || std::strcmp(name, "-net-world-lockstep-start-selftest") == 0) {
			s_FailTag = "net-world-lockstep-start-selftest";
			return TestWorldJoinLockstepStartDoesNotHoldTheSimUpdate();
		}
		if (std::strcmp(name, "typed-address") == 0 || std::strcmp(name, "-net-world-typed-address-selftest") == 0) {
			s_FailTag = "net-world-typed-address-selftest";
			return TestTypedAddressTargetsOnlyItsHost();
		}
		if (std::strcmp(name, "second-join") == 0 || std::strcmp(name, "-net-world-second-join-selftest") == 0) {
			s_FailTag = "net-world-second-join-selftest";
			return TestSecondJoinAtTheSameTickGetsTheImage();
		}
		if (std::strcmp(name, "image-queue") == 0 || std::strcmp(name, "-net-world-image-queue-selftest") == 0) {
			s_FailTag = "net-world-image-queue-selftest";
			return TestQueuedImageDoesNotReplaceTheOneInFlight();
		}
		if (std::strcmp(name, "h4-leave") == 0 || std::strcmp(name, "-net-world-h4-leave-selftest") == 0) {
			s_FailTag = "net-world-h4-leave-selftest";
			return TestH4CleanLeaveKeepsWorldSeatOpen();
		}
		if (std::strcmp(name, "directory") == 0 || std::strcmp(name, "-net-world-directory-selftest") == 0) {
			s_FailTag = "net-world-directory-selftest";
			return TestDirectoryRowResumeAndWorldBootBounds();
		}
		if (std::strcmp(name, "codec") == 0 || std::strcmp(name, "-net-world-codec-selftest") == 0) {
			s_FailTag = "net-world-codec-selftest";
			return TestWorldTransitionCodec();
		}
		if (std::strcmp(name, "ordinary-identity") == 0 || std::strcmp(name, "-net-world-ordinary-identity-selftest") == 0) {
			s_FailTag = "net-world-ordinary-identity-selftest";
			return TestOrdinaryIdentityStamps();
		}
		if (std::strcmp(name, "admit") == 0 || std::strcmp(name, "-net-world-admit-selftest") == 0) {
			s_FailTag = "net-world-admit-selftest";
			return TestDueActivationAdmits();
		}
		if (std::strcmp(name, "ordinary-start") == 0 || std::strcmp(name, "-net-world-ordinary-start-selftest") == 0) {
			s_FailTag = "net-world-ordinary-start-selftest";
			return TestOrdinaryStartStillRefusesEmptyRemotes();
		}
		if (std::strcmp(name, "stale") == 0 || std::strcmp(name, "-net-world-stale-selftest") == 0) {
			s_FailTag = "net-world-stale-selftest";
			return TestStaleWorldTransitionRefused();
		}
		if (std::strcmp(name, "catchup-hold") == 0 || std::strcmp(name, "-net-world-catchup-hold-selftest") == 0) {
			s_FailTag = "net-world-catchup-hold-selftest";
			return TestCatchUpHoldsUntilItsRoundRuns();
		}
		if (std::strcmp(name, "activate-brain") == 0 || std::strcmp(name, "-net-world-activate-brain-selftest") == 0) {
			s_FailTag = "net-world-activate-brain-selftest";
			return TestActivateNeverTakesAnotherMembersBrain();
		}
		if (std::strcmp(name, "image-publish") == 0 || std::strcmp(name, "-net-world-image-publish-selftest") == 0) {
			s_FailTag = "net-world-image-publish-selftest";
			return TestWorldImagePublishedFromTheWriter();
		}
		if (std::strcmp(name, "bootstrap") == 0 || std::strcmp(name, "-net-world-bootstrap-selftest") == 0) {
			s_FailTag = "net-world-bootstrap-selftest";
			return TestHostBootstrapRefusals();
		}
		if (std::strcmp(name, "concurrent") == 0 || std::strcmp(name, "-net-world-concurrent-selftest") == 0) {
			s_FailTag = "net-world-concurrent-selftest";
			return TestConcurrentJoinsKeepTheirOwnActivation();
		}
		if (std::strcmp(name, "overflow") == 0 || std::strcmp(name, "-net-world-overflow-selftest") == 0) {
			s_FailTag = "net-world-overflow-selftest";
			return TestSpectatorOverflowIsBounded();
		}
		if (std::strcmp(name, "capacity") == 0 || std::strcmp(name, "-net-world-capacity-selftest") == 0) {
			s_FailTag = "net-world-capacity-selftest";
			return TestWorldCapacityRidesTheV5Config();
		}
		if (std::strcmp(name, "ready-frame") == 0 || std::strcmp(name, "-net-world-ready-frame-selftest") == 0) {
			s_FailTag = "net-world-ready-frame-selftest";
			return TestReadyFramePackIncludesRemotes();
		}
		return Fail(std::string("unknown world-join case ") + name);
	}

	int NetWorldJoinSelfTest::Run() {
		s_FailTag = "net-world-join-selftest";
		if (const int result = TestIdentitySurvivesRestart(); result != 0) {
			return result;
		}
		if (const int result = TestFrameLogAndOffer(); result != 0) {
			return result;
		}
		if (const int result = TestImageBlobRoundTrip(); result != 0) {
			return result;
		}
		if (const int result = TestJoinerTransferPump(); result != 0) {
			return result;
		}
		if (const int result = TestCatchUpJoinerReport(); result != 0) {
			return result;
		}
		if (const int result = TestActivateBindingOnBothPeers(); result != 0) {
			return result;
		}
		if (const int result = TestCatchUpStopsAtTailGap(); result != 0) {
			return result;
		}
		if (const int result = TestCatchUpKeepsItsCeiling(); result != 0) {
			return result;
		}
		if (const int result = TestTakeDoesNotPumpTheSession(); result != 0) {
			return result;
		}
		if (const int result = TestCatchUpHoldsUntilItsRoundRuns(); result != 0) {
			return result;
		}
		if (const int result = TestActivateNeverTakesAnotherMembersBrain(); result != 0) {
			return result;
		}
		if (const int result = TestHostBootstrapRefusals(); result != 0) {
			return result;
		}
		if (const int result = TestWorldImagePublishedFromTheWriter(); result != 0) {
			return result;
		}
		if (const int result = TestImageWatermarkSurvivesAnEmptyCopy(); result != 0) {
			return result;
		}
		if (const int result = TestTicketRejoinTargetsTheWorld(); result != 0) {
			return result;
		}
		if (const int result = TestSpectatorActivationWaitsForItsImage(); result != 0) {
			return result;
		}
		if (const int result = TestSpectatorNeverTakesASeat(); result != 0) {
			return result;
		}
		if (const int result = TestSpectatorLobbyIdsRecycle(); result != 0) {
			return result;
		}
		if (const int result = TestTypedAddressTargetsOnlyItsHost(); result != 0) {
			return result;
		}
		if (const int result = TestWorldJoinLockstepStartDoesNotHoldTheSimUpdate(); result != 0) {
			return result;
		}
		if (const int result = TestSecondJoinAtTheSameTickGetsTheImage(); result != 0) {
			return result;
		}
		if (const int result = TestQueuedImageDoesNotReplaceTheOneInFlight(); result != 0) {
			return result;
		}
		if (const int result = TestSlowJoinerReannounceThenFree(); result != 0) {
			return result;
		}
		if (const int result = TestJoinPlaneAndLeave(); result != 0) {
			return result;
		}
		if (const int result = TestOrdinaryStartStillRefusesEmptyRemotes(); result != 0) {
			return result;
		}
		if (const int result = TestWorldStartsEmptyAndSurvivesLastLeave(); result != 0) {
			return result;
		}
		if (const int result = TestOrdinaryLiveJoinAdmission(); result != 0) {
			return result;
		}
		if (const int result = TestWorldLiveJoinAdmission(); result != 0) {
			return result;
		}
		if (const int result = TestH4CleanLeaveKeepsWorldSeatOpen(); result != 0) {
			return result;
		}
		if (const int result = TestDirectoryRowResumeAndWorldBootBounds(); result != 0) {
			return result;
		}
		if (const int result = TestWorldTransitionCodec(); result != 0) {
			return result;
		}
		if (const int result = TestOrdinaryIdentityStamps(); result != 0) {
			return result;
		}
		if (const int result = TestH4LeaveThenNewJoinSameHolder(); result != 0) {
			return result;
		}
		if (const int result = TestDueActivationAdmits(); result != 0) {
			return result;
		}
		if (const int result = TestStaleWorldTransitionRefused(); result != 0) {
			return result;
		}
		if (const int result = TestConcurrentJoinsKeepTheirOwnActivation(); result != 0) {
			return result;
		}
		if (const int result = TestSpectatorOverflowIsBounded(); result != 0) {
			return result;
		}
		if (const int result = TestWorldCapacityRidesTheV5Config(); result != 0) {
			return result;
		}
		if (const int result = TestReadyFramePackIncludesRemotes(); result != 0) {
			return result;
		}
		return Pass();
	}

	int NetWorldJoinSelfTest::RunCase(const char* name) {
		if (name == nullptr || name[0] == '\0') {
			return Run();
		}
		if (const int result = RunNamed(name); result != 0) {
			return result;
		}
		return Pass();
	}

} // namespace RTE
