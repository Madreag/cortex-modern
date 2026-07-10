#include "NetAdmissionSelfTest.h"

#include "LoopbackTransport.h"
#include "NetLockstep.h"
#include "NetSession.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace RTE {

	namespace {
		int Fail(const std::string& message) {
			std::cerr << "[net-admission-selftest] FAIL: " << message << std::endl;
			return 1;
		}

		// A transport peer id that never matches a committed seat's bound connection.
		constexpr NetPeerId c_UnboundPeer = 99;
		// The loopback assigns the single remote transport id 1 (see StartCoordinatorPair convention).
		constexpr NetPeerId c_BoundPeer = 1;

		// Pass-through transport that also lets the test push adversarial events into the poll stream,
		// so a real running match (with a genuine bound remote through the inner transport) can be
		// attacked by unbound-transport traffic the way an unauthenticated joiner would.
		class AdmissionProbeTransport : public INetTransport {
		public:
			explicit AdmissionProbeTransport(INetTransport& inner) : m_Inner(inner) {}
			bool StartHost(uint16_t port, std::string* error = nullptr) override { return m_Inner.StartHost(port, error); }
			bool Connect(const std::string& address, uint16_t port, std::string* error = nullptr) override { return m_Inner.Connect(address, port, error); }
			bool Send(NetPeerId peerId, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error = nullptr) override { return m_Inner.Send(peerId, lane, bytes, error); }
			void Disconnect(NetPeerId peerId, const std::string& reason) override { m_Inner.Disconnect(peerId, reason); }
			void Stop() override { m_Inner.Stop(); }
			uint32_t GetPeerPingMs(NetPeerId peerId) const override { return m_Inner.GetPeerPingMs(peerId); }
			std::vector<NetTransportEvent> PollEvents() override {
				std::vector<NetTransportEvent> out = m_Inner.PollEvents();
				for (NetTransportEvent& event: m_Injected) {
					out.push_back(std::move(event));
				}
				m_Injected.clear();
				return out;
			}
			void Inject(NetTransportEvent event) { m_Injected.push_back(std::move(event)); }

		private:
			INetTransport& m_Inner;
			std::vector<NetTransportEvent> m_Injected;
		};

		ControllerFrame MakeFrame(int64_t actorId, uint64_t stateMask) {
			ControllerFrame frame;
			frame.actorUniqueID = actorId;
			frame.stateMask = stateMask;
			frame.inputMode = static_cast<uint8_t>(Controller::CIM_PLAYER);
			frame.playerRaw = Players::PlayerOne;
			return frame;
		}

		NetLockstepConfig MakeConfig(uint8_t localPeerId, uint8_t remotePeerId, uint64_t sessionId, bool relay) {
			NetLockstepConfig config;
			config.sessionId = sessionId;
			config.startFrame = 0;
			config.inputDelayFrames = 0;
			config.timeoutMs = 1000000; // The test drives many steps; no missing-frame timeout should fire.
			config.localPeerId = localPeerId;
			config.remotePeerId = remotePeerId;
			config.remoteTransportPeerId = c_BoundPeer;
			config.peerCount = 2;
			config.relayToOtherPeers = relay; // Production hosts relay; it is the bound-vs-unbound discriminator.
			config.scenario = "AdmissionSelfTest";
			config.ownershipPolicy = "unique-id-split";
			return config;
		}

		NetTransportEvent GarbagePacket(NetPeerId from) {
			return {NetTransportEventType::PacketReceived, from, NetTransportLane::ControlReliable, {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11}, {}};
		}
		NetTransportEvent OversizedPacket(NetPeerId from) {
			return {NetTransportEventType::PacketReceived, from, NetTransportLane::ControlReliable, std::vector<uint8_t>(70000, 0x5A), {}};
		}
		// A packet with a valid lockstep header but no payload: decode-fails as a non-BadMagic error,
		// exercising the final Fail path (past the session/lobby BadMagic fallbacks).
		NetTransportEvent TruncatedLockstepPacket(NetPeerId from, uint64_t sessionId) {
			NetLockstepStart start;
			start.sessionId = sessionId;
			start.startFrame = 0;
			start.inputDelayFrames = 0;
			start.controllerFrameVersion = ControllerFrame::c_Version;
			start.controllerFrameEncodedSize = static_cast<uint16_t>(ControllerFrame::c_EncodedSize);
			start.localPeerId = 2;
			start.peerCount = 2;
			start.scenario = "AdmissionSelfTest";
			start.ownershipPolicy = "unique-id-split";
			std::vector<uint8_t> bytes;
			NetLockstepCodec::Encode({start}, bytes);
			if (bytes.size() > NetLockstepCodec::c_HeaderBytes) {
				bytes.resize(NetLockstepCodec::c_HeaderBytes);
			}
			return {NetTransportEventType::PacketReceived, from, NetTransportLane::ControlReliable, bytes, {}};
		}

		struct CoordinatorFixture {
			LoopbackTransport hostInner;
			LoopbackTransport clientTransport;
			AdmissionProbeTransport hostTransport{hostInner};
			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			uint64_t now = 0;

			bool BringToRunning(uint16_t port, uint64_t sessionId, std::string* error) {
				if (!hostTransport.StartHost(port, error)) return false;
				if (!clientTransport.Connect("loopback", port, error)) return false;
				if (!host.Start(hostTransport, MakeConfig(1, 2, sessionId, true), error)) return false;
				if (!client.Start(clientTransport, MakeConfig(2, 1, sessionId, false), error)) return false;
				for (int i = 0; i < 600 && !(host.IsRunning() && client.IsRunning()); ++i) {
					Step();
				}
				if (!host.IsRunning() || !client.IsRunning()) {
					if (error) *error = "coordinators did not reach Running";
					return false;
				}
				return true;
			}

			void Step() {
				host.Tick(now);
				client.Tick(now);
				hostInner.AdvanceTimeMs(5);
				clientTransport.AdvanceTimeMs(5);
				now += 5;
			}
		};

		// The running match must survive unbound-transport garbage and connection faults with its
		// commit stream unchanged, and still fail on a genuinely fatal local pump fault.
		int RunCoordinatorPlane() {
			const uint64_t sessionId = 0x7A00000000000001ULL;
			CoordinatorFixture fx;
			std::string error;
			if (!fx.BringToRunning(47001, sessionId, &error)) {
				return Fail("setup: " + error);
			}

			for (uint64_t producedFrame = 0; producedFrame < 5; ++producedFrame) {
				if (!fx.host.QueueLocalInput(producedFrame, {MakeFrame(100 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, {}, &error) ||
				    !fx.client.QueueLocalInput(producedFrame, {MakeFrame(200 + static_cast<int64_t>(producedFrame), producedFrame + 11)}, {}, &error)) {
					return Fail("queue input: " + error);
				}
			}

			std::vector<NetTransportEvent> attacks = {
				GarbagePacket(c_UnboundPeer),
				OversizedPacket(c_UnboundPeer),
				TruncatedLockstepPacket(c_UnboundPeer, sessionId),
				{NetTransportEventType::ConnectionFailed, c_InvalidNetPeerId, NetTransportLane::ControlReliable, {}, "unbound joiner bailed"},
				{NetTransportEventType::TransportError, c_InvalidNetPeerId, NetTransportLane::ControlReliable, {}, "accept failed"},
			};

			// Interleave the attack with the real commit stream so the proof is that the sim proceeds
			// undisturbed, not merely that the coordinator survives idle.
			std::vector<uint64_t> hostReady;
			std::vector<uint64_t> clientReady;
			size_t nextAttack = 0;
			// Run until BOTH the full commit stream landed AND every attack was injected+processed
			// (the frames can commit in the first tick or two, so the loop must not end early).
			for (int i = 0; i < 800 && !(hostReady.size() == 5 && clientReady.size() == 5 && nextAttack == attacks.size()); ++i) {
				if (nextAttack < attacks.size()) {
					fx.hostTransport.Inject(attacks[nextAttack++]);
				}
				fx.host.Tick(fx.now);
				fx.client.Tick(fx.now);
				NetLockstepReadyFrame ready;
				while (fx.host.PopReadyFrame(ready)) hostReady.push_back(ready.frame);
				while (fx.client.PopReadyFrame(ready)) clientReady.push_back(ready.frame);
				if (fx.host.IsFailed()) {
					return Fail("host match failed under admission attack: " + fx.host.BuildReportJson());
				}
				fx.hostInner.AdvanceTimeMs(5);
				fx.clientTransport.AdvanceTimeMs(5);
				fx.now += 5;
			}

			if (hostReady.size() != 5 || clientReady.size() != 5) {
				return Fail("match did not commit through the attack (host " + std::to_string(hostReady.size()) + " client " + std::to_string(clientReady.size()) + ")");
			}
			if (hostReady != clientReady) {
				return Fail("host and client committed different frames under attack");
			}
			if (fx.host.GetStats().ignoredAdmissionFaults != attacks.size()) {
				return Fail("unbound faults not all counted: " + std::to_string(fx.host.GetStats().ignoredAdmissionFaults));
			}
			if (fx.host.GetStats().framesAccepted != 5 || fx.client.GetStats().framesAccepted != 5) {
				return Fail("frame acceptance diverged under attack");
			}

			// Positive control: a committed peer's undecodable stream IS a genuine protocol error.
			{
				CoordinatorFixture bound;
				if (!bound.BringToRunning(47002, 0x7A00000000000002ULL, &error)) {
					return Fail("bound-control setup: " + error);
				}
				bound.hostTransport.Inject(GarbagePacket(c_BoundPeer));
				for (int i = 0; i < 20 && !bound.host.IsFailed(); ++i) bound.Step();
				if (!bound.host.IsFailed()) {
					return Fail("bound-peer garbage did not fail the match (over-broadened isolation)");
				}
			}

			// Positive control: our own transport pump breaking is genuinely fatal.
			{
				CoordinatorFixture local;
				if (!local.BringToRunning(47003, 0x7A00000000000003ULL, &error)) {
					return Fail("local-fault-control setup: " + error);
				}
				local.hostTransport.Inject({NetTransportEventType::LocalTransportFault, c_InvalidNetPeerId, NetTransportLane::ControlReliable, {}, "receive pump broke"});
				for (int i = 0; i < 20 && !local.host.IsFailed(); ++i) local.Step();
				if (!local.host.IsFailed()) {
					return Fail("local transport fault did not fail the match");
				}
			}

			return 0;
		}

		// The admission session must survive an unauthenticated joiner's connection fault, keep serving
		// everyone else, and still fail on a genuinely fatal local pump fault. A client's lone link
		// faulting stays fatal so a real join failure still surfaces fast.
		int RunSessionPlane() {
			{
				LoopbackTransport transport;
				NetSession session;
				NetSessionConfig config;
				config.port = 47010;
				config.maxPeers = 3;
				std::string error;
				if (!session.StartHost(transport, config, &error)) {
					return Fail("host session setup: " + error);
				}
				session.InjectEvent({NetTransportEventType::ConnectionFailed, c_InvalidNetPeerId, NetTransportLane::ControlReliable, {}, "joiner bailed"}, 10);
				session.InjectEvent({NetTransportEventType::TransportError, c_InvalidNetPeerId, NetTransportLane::ControlReliable, {}, "accept failed"}, 20);
				if (session.IsFailed()) {
					return Fail("host session failed on an unbound connection fault");
				}
				if (session.GetState() != NetSessionState::Listening) {
					return Fail("host session left the listening state under admission noise");
				}
				if (session.GetStats().unboundConnectionFaults != 2) {
					return Fail("host session did not count the unbound faults: " + std::to_string(session.GetStats().unboundConnectionFaults));
				}
				// Positive control: our own pump breaking IS fatal, even in the admission role.
				session.InjectEvent({NetTransportEventType::LocalTransportFault, c_InvalidNetPeerId, NetTransportLane::ControlReliable, {}, "receive pump broke"}, 30);
				if (!session.IsFailed()) {
					return Fail("host session survived a local transport fault");
				}
			}

			{
				LoopbackTransport hostTransport;
				LoopbackTransport clientTransport;
				std::string error;
				if (!hostTransport.StartHost(47011, &error)) {
					return Fail("client-control host: " + error);
				}
				NetSession client;
				NetSessionConfig config;
				config.port = 47011;
				if (!client.StartClient(clientTransport, "loopback", config, &error)) {
					return Fail("client session setup: " + error);
				}
				client.InjectEvent({NetTransportEventType::ConnectionFailed, c_InvalidNetPeerId, NetTransportLane::ControlReliable, {}, "host unreachable"}, 10);
				if (!client.IsFailed()) {
					return Fail("client session did not fail on its lone-link fault");
				}
			}

			return 0;
		}
	} // namespace

	int NetAdmissionSelfTest::Run() {
		if (const int result = RunCoordinatorPlane(); result != 0) {
			return result;
		}
		if (const int result = RunSessionPlane(); result != 0) {
			return result;
		}
		std::cout << "[net-admission-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
