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
#include "NetLobbySession.h"
#include "NetWorldJoin.h"
#include "System/ScenarioRunner.h"

#include "nlohmann/json.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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

		std::filesystem::path LaneDirectory() {
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
			if (!ResetLane(&error)) {
				return Fail(error);
			}
			const std::string path = (LaneDirectory() / "persistent.identity").string();
			NetWorldIdentity first;
			if (!NetWorldIdentityFile::OpenForBoot(path, first, &error) || !first.IsValid()) {
				return Fail("first boot did not publish a world identity: " + error);
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
				return Fail("published identity did not round-trip on disk");
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
				return Fail("identity encode did not round-trip");
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

		int TestLiveJoinAdmission() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			const NetH4Identity identity = MakeH4Identity();
			const std::vector<NetH4Seat> seats = {{0, 0, 0, false, 1, true}, {1, 1, 1, false, 2, false}};

			NetH4NewJoin join;
			join.identity = identity;
			join.displayName = "alice";
			join.txId.fill(9);

			{
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
			}

			{
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
			const uint64_t nowFrame = 80;
			uint64_t activation = 0;
			if (!host.NoteCatchUpProgress(7, 40, 20, 50, nowFrame, &activation, &error) || activation != nowFrame + c_NetWorldActivationLeadFrames) {
				return Fail("catch-up did not announce E: " + error);
			}
			const NetLobbyStateChunk report = MakeWorldJoinReport(c_NetWorldReportCatchUp, activation - 1);
			uint8_t kind = 0;
			uint64_t value = 0;
			if (!ParseWorldJoinReport(report, kind, value) || kind != c_NetWorldReportCatchUp || value != activation - 1) {
				return Fail("production joiner report did not carry appliedThrough");
			}
			if (!host.NoteCatchUpProgress(7, value, value > 40 ? value - 40 : 0, 10, nowFrame, nullptr, &error)) {
				return Fail("DriveWorldJoins report.value was refused: " + error);
			}
			if (host.FindSession(7)->acknowledgedThrough != activation - 1) {
				return Fail("appliedThrough-did-not-reach-E-minus-1");
			}
			if (host.DueActivation(activation) != nullptr) {
				return Fail("DueActivation stayed true at E");
			}
			if (host.DueActivation(activation - 1) == nullptr) {
				return Fail("DueActivation did not fire only at E-1");
			}

			std::vector<NetLockstepFrame> tail = {MakeCommittedFrame(41), MakeCommittedFrame(42)};
			if (!ScenarioRunner::InstallWorldCatchUp(40, tail, &error)) {
				return Fail("catch-up install: " + error);
			}
			ScenarioRunner::SetWorldCatchUpActivation(43);
			NetLockstepReadyFrame ready;
			if (!ScenarioRunner::TakeWorldCatchUpReadyFrame(42, ready, &error) || ready.frame != 42) {
				return Fail("TakeWorldCatchUpReadyFrame did not apply targetFrame == simTick");
			}
			if (ScenarioRunner::WorldCatchUpActive()) {
				return Fail("catch-up apply flag did not clear at E-1");
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
				return Fail("activate-binding-missing");
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
			NetLockstepFrame joinerView;
			if (!NetLockstepCodec::DecodeRecoveryInput(recovery, hostView, nullptr) ||
			    !NetLockstepCodec::DecodeRecoveryInput(recovery, joinerView, nullptr)) {
				return Fail("activate-binding-missing: a peer could not decode the activate at E");
			}
			const auto* hostApplied = hostView.commands.empty() ? nullptr : std::get_if<NetGameWorldTransition>(&hostView.commands[0].payload);
			const auto* joinerApplied = joinerView.commands.empty() ? nullptr : std::get_if<NetGameWorldTransition>(&joinerView.commands[0].payload);
			if (hostApplied == nullptr || joinerApplied == nullptr || !hostApplied->bindBrain || !joinerApplied->bindBrain ||
			    hostApplied->activationFrame != e || joinerApplied->activationFrame != e) {
				return Fail("activate-binding-missing");
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
			return TestLiveJoinAdmission();
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
		if (const int result = TestLiveJoinAdmission(); result != 0) {
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
