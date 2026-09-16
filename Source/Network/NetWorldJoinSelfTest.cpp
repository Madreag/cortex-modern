#include "NetWorldJoinSelfTest.h"

#include "ControllerFrame.h"
#include "LoopbackTransport.h"
#include "NetAuthCrypto.h"
#include "NetLockstep.h"
#include "NetMatchConfig.h"
#include "NetReconnectAdmission.h"
#include "NetReconnectSession.h"
#include "NetSeatAuth.h"
#include "NetLobbySession.h"
#include "NetWorldJoin.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <variant>

namespace RTE {

	namespace {
		int Fail(const std::string& message) {
			std::cerr << "[net-world-join-selftest] FAIL: " << message << std::endl;
			return 1;
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
			if (host.DueActivation(activation - 1) != nullptr) {
				return Fail("activation was due before the announced tick");
			}
			if (host.DueActivation(activation) == nullptr || !host.CompleteActivation(7, activation, &error)) {
				return Fail("activation at E was refused: " + error);
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
			if (!host.BeginJoin(8, 3, "bob", 2000, &error) || host.FindSession(8) == nullptr || host.FindSession(8)->assignedPeerId != 2) {
				return Fail("rejoin after a clean leave did not land in the running world");
			}

			if (!host.BeginJoin(9, 4, "carol", 3000, &error) || host.FindSession(9) == nullptr || !host.FindSession(9)->spectator) {
				return Fail("overflow did not spectate");
			}
			if (host.ExpireStaleJoins(3000 + c_NetWorldJoinDeadlineMs + 1) == 0) {
				return Fail("a stalled bootstrap did not expire");
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
				return Fail("lockstep peer identity is invalid");
			}
			if (error != "lockstep peer identity is invalid") {
				return Fail("lockstep peer identity is invalid");
			}
			config.remotePeerId = 2;
			error.clear();
			if (ordinary.Start(transport, config, &error)) {
				return Fail("lockstep peer identity is invalid");
			}
			if (error != "lockstep peer identity is invalid") {
				return Fail("lockstep peer identity is invalid");
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

			NetLockstepCoordinator ordinary;
			NetLockstepConfig ordinaryConfig = worldConfig;
			ordinaryConfig.matchConfig.persistentWorld = false;
			ordinaryConfig.matchConfig.worldId.clear();
			ordinaryConfig.matchConfig.worldBoot = 0;
			ordinaryConfig.remotePeerId = 2;
			error.clear();
			if (ordinary.Start(hostTransport, ordinaryConfig, &error)) {
				return Fail("lockstep has no remote transport targets");
			}
			if (error != "lockstep peer identity is invalid" && error != "lockstep has no remote transport targets") {
				return Fail("lockstep has no remote transport targets");
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
					return Fail("the match is already in progress");
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
				bool offered = false;
				for (const NetH4Outbound& outbound: host.TakeOutbound()) {
					if (std::holds_alternative<NetH4TicketOffer>(outbound.payload)) {
						offered = true;
					}
					if (const auto* rejected = std::get_if<NetJoinRejected>(&outbound.payload)) {
						if (rejected->humanMessage == "the match is already in progress") {
							return Fail("a fresh join into a running world was refused as a live match");
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

		int TestCatchUpAppliedThroughAndBinding() {
			std::string error;
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
			if (!host.NoteCatchUpProgress(7, nowFrame, nowFrame, 1, nowFrame, &activation, &error)) {
				return Fail("catch-up with nowFrame as appliedThrough was the old lie; it must still accept a number: " + error);
			}
			if (host.FindSession(7)->acknowledgedThrough != nowFrame) {
				return Fail("appliedThrough-was-nowFrame");
			}
			if (!host.NoteCatchUpProgress(7, 40, 1, 1, nowFrame, nullptr, &error)) {
				// going backwards must fail
			} else {
				return Fail("a catch-up acknowledgement cannot go backwards");
			}
			if (!host.NoteCatchUpProgress(7, activation - 1, 1, 1, activation, nullptr, &error)) {
				return Fail("catch-up refused appliedThrough == E-1: " + error);
			}
			if (host.FindSession(7)->acknowledgedThrough != activation - 1) {
				return Fail("appliedThrough-did-not-reach-E-minus-1");
			}
			if (host.DueActivation(activation) == nullptr) {
				return Fail("activation at E with appliedThrough == E-1 was not due");
			}
			const NetGameWorldTransition transition = BuildWorldActivateTransition(*host.FindSession(7), MakeWorldConfig(), host.Membership().Revision());
			if (transition.className.empty() || transition.preset != "Brain Robot" || !transition.bindBrain ||
			    transition.player < 0 || transition.peerId != 2) {
				return Fail("activate-binding-missing");
			}
			NetLobbyStateChunk report = MakeWorldJoinReport(c_NetWorldReportCatchUp, activation - 1);
			uint8_t kind = 0;
			uint64_t value = 0;
			if (!ParseWorldJoinReport(report, kind, value) || kind != c_NetWorldReportCatchUp || value != activation - 1) {
				return Fail("catch-up report did not round-trip");
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

	int NetWorldJoinSelfTest::Run() {
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
		if (const int result = TestCatchUpAppliedThroughAndBinding(); result != 0) {
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
		std::cout << "[net-world-join-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
