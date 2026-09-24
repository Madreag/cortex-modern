#include "NetRejoinMatrixSelfTest.h"

#include "ControllerFrame.h"
#include "LoopbackTransport.h"
#include "NetGameCommand.h"
#include "NetIdentity.h"
#include "NetLockstep.h"
#include "NetMatchConfig.h"
#include "NetProtocol.h"
#include "NetSession.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace RTE {

	namespace {
		constexpr const char* c_Tag = "[net-rejoin-matrix-selftest]";

		enum class State : uint8_t { Active, Held, Reclaiming, RejoinConnecting, RejoinImagePending, RejoinLoading, RejoinTailReplay, Parked, Relaunching, Migrating, Draining, Left, Count };
		enum class Event : uint8_t {
			HoldProposed, HoldResolved, ParkBegin, ParkEnd, PrivateCaptureComplete, WorldImageOffered, OpeningResumeOffer, TailReplayComplete, ResyncRelaunch,
			HostGoodbye, HostLost, MigrationBegin, MigrationComplete, MigrationFail, SuccessorLost, LateJoin, TicketRejoin, HeldRejoin, Kick, Ban,
			SeatRelease, OwnCap, ResumeFromDisk, MatchOver, LinkBlip, LinkRestore, Count
		};

		const char* StateName(State state) {
			switch (state) {
				case State::Active: return "Active";
				case State::Held: return "Held";
				case State::Reclaiming: return "Reclaiming";
				case State::RejoinConnecting: return "Rejoin:Connecting";
				case State::RejoinImagePending: return "Rejoin:ImagePending";
				case State::RejoinLoading: return "Rejoin:Loading";
				case State::RejoinTailReplay: return "Rejoin:TailReplay";
				case State::Parked: return "Parked";
				case State::Relaunching: return "Relaunching";
				case State::Migrating: return "Migrating";
				case State::Draining: return "Draining";
				case State::Left: return "Left";
				default: return "?";
			}
		}

		const char* EventName(Event event) {
			switch (event) {
				case Event::HoldProposed: return "hold-proposed";
				case Event::HoldResolved: return "hold-resolved";
				case Event::ParkBegin: return "park-begin";
				case Event::ParkEnd: return "park-end";
				case Event::PrivateCaptureComplete: return "private-capture-complete";
				case Event::WorldImageOffered: return "world-image-offered";
				case Event::OpeningResumeOffer: return "opening-resume-offer";
				case Event::TailReplayComplete: return "tail-replay-complete";
				case Event::ResyncRelaunch: return "resync-relaunch";
				case Event::HostGoodbye: return "host-goodbye";
				case Event::HostLost: return "host-lost";
				case Event::MigrationBegin: return "migration-begin";
				case Event::MigrationComplete: return "migration-complete";
				case Event::MigrationFail: return "migration-fail";
				case Event::SuccessorLost: return "migration-successor-lost";
				case Event::LateJoin: return "late-join";
				case Event::TicketRejoin: return "ticket-rejoin";
				case Event::HeldRejoin: return "held-rejoin";
				case Event::Kick: return "kick";
				case Event::Ban: return "ban";
				case Event::SeatRelease: return "seat-release";
				case Event::OwnCap: return "own-cap";
				case Event::ResumeFromDisk: return "resume-from-disk";
				case Event::MatchOver: return "match-over";
				case Event::LinkBlip: return "link-blip";
				case Event::LinkRestore: return "link-restore";
				default: return "?";
			}
		}

		bool IsRejoin(State state) { return state == State::RejoinConnecting || state == State::RejoinImagePending || state == State::RejoinLoading || state == State::RejoinTailReplay; }

		bool IsTierTwoEvent(Event event) {
			return event == Event::WorldImageOffered || event == Event::OpeningResumeOffer || event == Event::MigrationBegin || event == Event::MigrationComplete ||
			       event == Event::MigrationFail || event == Event::SuccessorLost || event == Event::LateJoin || event == Event::ResumeFromDisk;
		}

		int Tier(State state, Event event) { return state == State::Migrating || IsTierTwoEvent(event) ? 2 : 1; }

		const char* PhaseOf(State state) {
			switch (state) {
				case State::RejoinConnecting: return "Connecting";
				case State::RejoinImagePending: return "ImagePending";
				case State::RejoinLoading: return "Loading";
				case State::RejoinTailReplay: return "TailReplay";
				default: return "Active";
			}
		}

		/// One pair's row: what the policy expects, where it says so, the gap when it does not, and why a pair is not walked.
		struct Expectation {
			std::string expect;
			std::string source;
			std::string gap;
			std::string notWalked;
			std::string note;
		};

		constexpr const char* c_LastHumanGap = "a round whose last remote human is removed or released: no ruling says whether it ends; the conservative expectation keeps it running";

		Expectation Expect(State s, Event e) {
			Expectation x;
			const bool rejoin = IsRejoin(s);
			const bool heldLike = s == State::Held || s == State::Left || rejoin;
			const bool activeLike = s == State::Active || s == State::Parked;
			// The client's session reading when the event does not concern it: a rejoin keeps its phase, a handshake keeps going.
			const std::string quiet = s == State::RejoinConnecting ? "sess=alive" : rejoin ? std::string("sess=phase:") + PhaseOf(s) : "sess=ready";
			const std::string held = s == State::Left ? "seat=Held peer=left" : rejoin ? "seat=Held" : "seat=Held peer=held";
			const auto set = [&x](std::string expect, std::string source, std::string gap = {}) {
				x.expect = std::move(expect);
				x.source = std::move(source);
				x.gap = std::move(gap);
			};
			const std::string serviceStep = "a NetMatchService step (private capture writer, world join, checkpoint resume) that needs an activity load; the in-process rig has none";

			if (s == State::Migrating) {
				x.notWalked = "needs a three-peer rig with a successor in migration; not composed in this row";
				switch (e) {
					case Event::MigrationComplete: set("legal: the successor hosts; a held seat stays with the AI on the migrated host and rejoins it through resync", "R1-392ii"); break;
					case Event::MigrationFail: set("legal: the survivors leave to the landing with 'The host left the match'", "R1-392ii"); break;
					case Event::SuccessorLost: set("legal: the migration moves to the next successor", "DESIGN-MIGRATION"); break;
					case Event::MigrationBegin: set("ignore: a migration already running is not restarted", "DESIGN-MIGRATION"); break;
					case Event::HostGoodbye:
					case Event::HostLost: set("ignore: the host is already gone", "R1-392ii"); break;
					case Event::MatchOver: set("refuse: no host remains to end the match until the successor hosts", "GAP", "an end of match requested while the host is being replaced"); break;
					default: set("ignore until the migration completes or fails (conservative)", "GAP", "an event other than the migration's own steps arriving while the host is being replaced"); break;
				}
				return x;
			}

			switch (e) {
				case Event::HoldProposed:
					if (s == State::Active) set("seat=Held round=run peer=held holds>0 " + quiet, "RB2");
					else if (s == State::Parked) set("seat=Held round=run peer=held " + quiet, "RB2 LS-PARK");
					else if (s == State::Draining) set("seat=Active round=run holds=0 " + quiet, "LS-DRAIN");
					else if (heldLike) set(held + " round=run holds=0 " + quiet, "RB2");
					else if (s == State::Reclaiming) set("seat=Reclaiming round=run " + quiet, "GAP", "a hold proposed for a seat whose reclaim is agreed but not active yet (nothing is owed by it before E)");
					else set("api=refused round=relaunch " + quiet, "LS-STOP");
					break;
				case Event::HoldResolved:
					if (activeLike || s == State::Draining) set("seat=Active round=run holds=0 " + quiet, "LS-STOP");
					else if (heldLike) set(held + " round=run " + quiet, "RB3 R2D3");
					else if (s == State::Reclaiming) set("seat=Reclaiming round=run " + quiet, "RB3 R2D3");
					else set("round=relaunch " + quiet, "LS-STOP");
					break;
				case Event::ParkBegin:
					if (activeLike) set("seat=Active round=run peer=run holds=0 " + quiet, "LS-PARK");
					else if (s == State::Draining) set("round=run holds=0 " + quiet, "GAP", "a capture park opened after the round's last tick");
					else if (heldLike) set(held + " round=run holds=0 " + quiet, "LS-PARK");
					else if (s == State::Reclaiming) set("seat=Reclaiming round=run holds=0 " + quiet, "C5102");
					else set("round=relaunch " + quiet, "LS-STOP");
					break;
				case Event::ParkEnd:
					if (s == State::Parked) set("seat=Active round=run peer=run holds=0 " + quiet, "LS-PARK");
					else if (s == State::Active) set("seat=Active round=run peer=run holds=0 " + quiet, "GAP", "a park end with no park open");
					else if (s == State::Draining) set("round=run holds=0 " + quiet, "GAP", "a park end with no park open");
					else if (heldLike) set(held + " round=run holds=0 " + quiet, "GAP", "a park end with no park open");
					else if (s == State::Reclaiming) set("seat=Reclaiming round=run " + quiet, "GAP", "a park end with no park open");
					else set("round=relaunch " + quiet, "LS-STOP");
					break;
				case Event::PrivateCaptureComplete:
					x.notWalked = serviceStep;
					if (s == State::RejoinImagePending) set("legal: ImagePending -> Loading on the newest base image", "R2-206 R2D2");
					else if (s == State::Held) set("ignore: the base image is kept for the seat's next rejoin (the steady-cost refresh rule)", "R2-206");
					else set("ignore: no seat waits on that image (conservative)", "GAP", "a private image completing for a seat that is not waiting on one");
					break;
				case Event::WorldImageOffered:
					x.notWalked = serviceStep;
					if (s == State::RejoinImagePending) set("legal: ImagePending -> Loading on the newest image; coverage starts at the loaded image", "R2D2");
					else if (s == State::RejoinLoading || s == State::RejoinTailReplay) set("ignore: the load in progress finishes first (conservative)", "GAP", "a newer world image offered while an older one loads or replays");
					else set("ignore: no seat waits on an image (conservative)", "GAP", "a world image offered to a seat that is not waiting on one");
					break;
				case Event::OpeningResumeOffer:
					x.notWalked = serviceStep;
					set("refuse: the opening resume offer is retired once the round runs past its anchor; a rejoin takes the image path with the newest image", "R2WAY2");
					break;
				case Event::TailReplayComplete:
					x.notWalked = serviceStep;
					if (s == State::RejoinTailReplay) set("legal: TailReplay -> Active; the seat plays live from its activation", "R2D2 RB3");
					else set("n/a: the completion is raised only by the seat's own tail replay", "NS-PHASE");
					break;
				case Event::ResyncRelaunch:
					if (activeLike) set("round=relaunch peer=relaunch " + quiet, "LS-STOP");
					else if (s == State::Draining) set("round=run " + quiet, "GAP", "a relaunch requested after the round's last tick");
					else if (s == State::Held || s == State::Left) set("round=relaunch seat=Held " + quiet, "LS-STOP");
					else if (s == State::Reclaiming) set("round=relaunch " + quiet, "LS-STOP", "what an agreed reclaim becomes across a relaunch is not ruled");
					else if (rejoin) set("round=relaunch " + quiet, "LS-STOP", "a relaunch while the seat's rejoin is in flight: the conservative expectation keeps the rejoin's session");
					else set("round=relaunch " + quiet, "LS-STOP");
					break;
				case Event::HostGoodbye:
					if (activeLike || s == State::Draining) set("round=ended peer=noresync sess=ended", "R1-392ii R2-392 H4-7");
					else if (s == State::Held || s == State::Reclaiming) set("round=ended peer=held sess=ended", "R1-392ii H4-7");
					else if (s == State::Left) set("round=ended peer=left sess=ended", "R1-392ii H4-7");
					else if (rejoin) set("round=ended sess=ended", "NS-PHASE R1-392ii");
					else set("sess=ended", "H4-7 R1-392ii");
					break;
				case Event::HostLost:
					if (activeLike || s == State::Draining) set("peer=noresync sess=ended", "R1-392ii R2-392");
					else if (s == State::Held || s == State::Reclaiming) set("peer=held sess=ended", "R1-392ii");
					else if (s == State::Left) set("peer=left sess=ended", "R1-392ii");
					else if (rejoin) set("sess=ended", "NS-PHASE");
					else set("sess=ended", "R1-392ii");
					break;
				case Event::MigrationBegin:
					if (s == State::Relaunching) set("api=refused round=relaunch", "R1-392ii");
					else set("api=refused round=run", "R1-392ii");
					break;
				case Event::MigrationComplete:
				case Event::MigrationFail:
				case Event::SuccessorLost:
					x.notWalked = "needs a migration in progress (three peers); not composed in this row";
					set("n/a: raised only while a host migration runs", "DESIGN-MIGRATION");
					break;
				case Event::LateJoin:
					if (s == State::Relaunching) set("api=refused round=relaunch", "H4-7 R2D3");
					else set("api=refused round=run " + quiet, "H4-7 R2D3");
					break;
				case Event::TicketRejoin:
					if (activeLike) set("seat=Active round=run peer=run holds=0 " + quiet, "H4-0 H4-6");
					else if (s == State::Draining) set("round=run holds=0 " + quiet, "H4-0");
					else if (heldLike) set(held + " round=run " + quiet, "H4-0");
					else if (s == State::Reclaiming) set("seat=Reclaiming round=run " + quiet, "H4-0");
					else set("round=relaunch " + quiet, "H4-0");
					break;
				case Event::HeldRejoin:
					if (activeLike || s == State::Draining) set("api=refused seat=Active round=run " + quiet, "H4-4 R2D3");
					else if (s == State::Held || rejoin) set("api=ok seat=Reclaiming round=run " + quiet, "RB3 R2D3");
					else if (s == State::Left) set("api=refused seat=Held round=run " + quiet, "GAP", "a clean leaver's return: H4 section 7 revokes its ticket, while R1 F7 and R2 D3 hand a left seat to the AI and let a held seat reclaim; walked at the coordinator, where the admission plane's ticket check is not composed");
					else if (s == State::Reclaiming) set("api=ok seat=Reclaiming round=run " + quiet, "H4-6");
					else set("api=refused round=relaunch " + quiet, "GAP", "a returner arriving during a relaunch (conservative: the running round refuses; the relaunch's own admission carries it)");
					break;
				case Event::Kick:
				case Event::Ban: {
					const std::string reason = e == Event::Kick ? "ParticipantRemoved" : "ParticipantBanned";
					if (s == State::Relaunching) set("round=relaunch sess=ended:" + reason, "NP-KICK");
					else if (s == State::RejoinConnecting) {
						set("seat=Left round=run", "NP-KICK LS-STOP", c_LastHumanGap);
						x.note = "coordinator half only: the handshaking returner is refused by the admission plane (NetReconnectHost), which the rig does not compose";
					} else set("seat=Left round=run sess=ended:" + reason, "NP-KICK LS-STOP", c_LastHumanGap);
					break;
				}
				case Event::SeatRelease:
					if (activeLike || s == State::Draining) set("seat=Active round=run holds=0 " + quiet, "LS-STOP");
					else if (s == State::Held || s == State::Left) set("seat=Left round=run " + quiet, "LS-STOP", c_LastHumanGap);
					else if (rejoin) set("seat=Left round=run", "LS-STOP", std::string(c_LastHumanGap) + "; and nothing rules what a rejoining client whose seat was released is told");
					else if (s == State::Reclaiming) set("seat=Reclaiming round=run " + quiet, "GAP", "a release of a seat whose reclaim is agreed");
					else set("round=relaunch " + quiet, "LS-STOP");
					break;
				case Event::OwnCap:
					if (activeLike) set("seat=Held round=run sess=ended", "R1F7", "R1 F7 rules a client's own end for a persistent world; for a match the conservative reading keeps the round running and hands the seat to the AI as a leave does");
					else if (s == State::Draining) set("peer=over sess=ended", "LS-DRAIN");
					else if (heldLike) set("seat=Held round=run sess=ended", "R1F7");
					else if (s == State::Reclaiming) set("seat=Reclaiming round=run sess=ended", "GAP", "a returner that reaches its own cap before its activation frame");
					else set("round=relaunch sess=ended", "R1F7");
					break;
				case Event::ResumeFromDisk:
					x.notWalked = serviceStep;
					if (s == State::Relaunching) set("legal: the relaunch loads the match's newest checkpoint from disk on every peer", "READY-4");
					else set("refuse: a running round is not replaced by a disk resume (conservative)", "GAP", "a resume from disk requested while a round runs");
					break;
				case Event::MatchOver:
					if (activeLike || s == State::Draining) set("round=over peer=over sess=ended", "LS-STOP NS-PHASE");
					else if (s == State::Relaunching) set("sess=ended", "NS-PHASE");
					else set("round=over sess=ended", "NS-PHASE");
					break;
				case Event::LinkBlip:
				case Event::LinkRestore: {
					const std::string source = e == Event::LinkBlip ? "RB2" : "RB3";
					if (activeLike) set("seat=Active round=run peer=run holds=0 " + quiet, source);
					else if (s == State::Draining) set("round=run holds=0 " + quiet, "LS-DRAIN");
					else if (heldLike) set(held + " round=run " + quiet, "RB3");
					else if (s == State::Reclaiming) set("seat=Reclaiming round=run " + quiet, "RB3");
					else set("round=relaunch " + quiet, "NS-PHASE");
					break;
				}
				default: break;
			}
			return x;
		}

		NetHash32 SeededHash(uint8_t seed) {
			NetHash32 hash{};
			for (size_t i = 0; i < hash.size(); ++i) hash[i] = static_cast<uint8_t>(seed + i);
			return hash;
		}

		NetSessionConfig SessionConfig(uint16_t port, uint64_t nonce, const std::string& name) {
			NetSessionConfig config;
			config.localIdentity.gameVersion = "7.0.0-test";
			config.localIdentity.networkProtocolVersion = NetProtocol::c_Version;
			config.localIdentity.controllerFrameVersion = ControllerFrame::c_Version;
			config.localIdentity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
			config.localIdentity.buildId = "rejoin-matrix-selftest";
			config.localIdentity.platform = "test";
			config.localIdentity.deterministicConfigHash = SeededHash(1);
			config.localIdentity.moduleManifestHash = SeededHash(33);
			config.localIdentity.sessionRulesHash = SeededHash(65);
			config.localIdentity.sessionIdentityHash = SeededHash(97);
			config.displayName = name;
			config.port = port;
			config.sessionId = 0x524A000000000000ULL + port;
			config.localNonce = nonce;
			config.maxPeers = 1;
			config.heartbeatIntervalMs = 50;
			config.timeoutMs = 1000;
			return config;
		}

		NetLockstepConfig CoordinatorConfig(uint8_t local, uint8_t remote, uint64_t session) {
			NetLockstepConfig config;
			config.sessionId = session;
			config.roundId = session;
			config.startFrame = 0;
			config.inputDelayFrames = 0;
			config.timeoutMs = 20000;
			config.localPeerId = local;
			config.remotePeerId = remote;
			config.peerCount = 2;
			config.remoteTransportPeerId = 1;
			config.frameLane = NetTransportLane::ControlReliable;
			config.scenario = "RejoinMatrixSelfTest";
			config.ownershipPolicy = "unique-id-split";
			config.substituteSlowPeers = true;
			config.simTickMs = 1000.0 / 60.0;
			return config;
		}

		/// A host (peer 1) and one client (peer 2) on a bounded-wait round, with the same pair on a session plane. The client is the subject seat.
		struct Rig {
			uint16_t port = 0;
			LoopbackTransport hostWire;
			LoopbackTransport clientWire;
			LoopbackTransport hostSessionWire;
			std::unique_ptr<LoopbackTransport> clientSessionWire = std::make_unique<LoopbackTransport>();
			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			NetSession hostSession;
			std::unique_ptr<NetSession> clientSession = std::make_unique<NetSession>();
			uint64_t now = 0;
			uint64_t nextHostSimMs = 0;
			uint64_t hostSimulated = 0;
			uint64_t clientSimulated = 0;
			uint64_t hostQueued = 0;
			uint64_t clientQueued = 0;
			uint64_t finalFrame = UINT64_MAX;
			uint64_t parkCompleted = UINT64_MAX;
			uint64_t autoCompleteAtMs = UINT64_MAX;
			bool hostLive = true;
			bool clientLive = true;
			bool clientFeeding = true;
			bool waiting = false;
			uint32_t holdsAtEntry = 0;
			std::string error;
		};

		uint32_t HoldsOfSeat(const Rig& r) {
			const auto& peers = r.host.GetStats().peers;
			const auto found = peers.find(2);
			return found == peers.end() ? 0U : found->second.holds;
		}

		void Step(Rig& r) {
			if (r.hostLive) { r.hostWire.AdvanceTimeMs(1); r.hostSessionWire.AdvanceTimeMs(1); }
			if (r.clientLive) { r.clientWire.AdvanceTimeMs(1); r.clientSessionWire->AdvanceTimeMs(1); }
			// A capture takes its time on every peer and then reports; the park is the window around it.
			if (r.autoCompleteAtMs != UINT64_MAX && r.now >= r.autoCompleteAtMs && r.parkCompleted != UINT64_MAX) {
				r.host.CompleteSynchronizedCapture(r.parkCompleted, 60.0);
				r.client.CompleteSynchronizedCapture(r.parkCompleted, 60.0);
				r.autoCompleteAtMs = UINT64_MAX;
			}
			std::string ignored;
			if (r.hostLive && r.host.IsRunning()) {
				for (; r.hostQueued <= std::min(r.hostSimulated + 6, r.finalFrame); ++r.hostQueued) if (!r.host.QueueLocalInput(r.hostQueued, {}, {}, &ignored)) break;
			}
			if (r.clientLive && r.clientFeeding && r.client.IsRunning()) {
				for (; r.clientQueued <= std::min(r.clientSimulated + 6, r.finalFrame); ++r.clientQueued) if (!r.client.QueueLocalInput(r.clientQueued, {}, {}, &ignored)) break;
			}
			if (r.hostLive) { r.host.Tick(r.now); r.hostSession.Tick(r.now); }
			if (r.clientLive) { r.client.Tick(r.now); r.clientSession->Tick(r.now); }
			NetLockstepReadyFrame ready;
			// The host simulates one frame a tick at the sim's rate and waits like a live sim, so its runway drains the way a real one does.
			if (r.hostLive && r.now >= r.nextHostSimMs && r.hostSimulated < r.finalFrame) {
				if (r.host.PopReadyFrame(ready)) {
					if (r.waiting) { r.host.FinishFrameWait(r.now); r.waiting = false; }
					(void)r.host.FinishSimulationTick(ready.frame);
					r.hostSimulated = ready.frame;
					r.nextHostSimMs = r.now + 17;
				} else if (r.hostSimulated > 0 && r.host.IsRunning()) {
					r.waiting = true;
					(void)r.host.NoteFrameWait(r.hostSimulated + 1, r.now);
				}
			}
			if (r.clientLive) {
				while (r.clientSimulated < r.finalFrame && r.client.PopReadyFrame(ready)) {
					(void)r.client.FinishSimulationTick(ready.frame);
					r.clientSimulated = ready.frame;
				}
			}
			++r.now;
		}

		bool Pump(Rig& r, uint64_t ms, const std::function<bool()>& until = {}) {
			for (uint64_t i = 0; i < ms; ++i) {
				if (until && until()) return true;
				Step(r);
			}
			return until ? until() : true;
		}

		bool Fail(Rig& r, const std::string& message) {
			r.error = message + " (host=" + NetLockstepCoordinator::StateName(r.host.GetState()) + " \"" + r.host.GetStats().timeoutReason + "\" client=" +
			          NetLockstepCoordinator::StateName(r.client.GetState()) + " \"" + r.client.GetStats().timeoutReason + "\" host_sim=" + std::to_string(r.hostSimulated) +
			          " client_sim=" + std::to_string(r.clientSimulated) + ")";
			return false;
		}

		bool StartRig(Rig& r, uint64_t session) {
			std::string error;
			if (!r.hostWire.StartHost(r.port, &error) || !r.clientWire.Connect("loopback", r.port, &error)) return Fail(r, "lockstep loopback: " + error);
			NetLockstepConfig hostConfig = CoordinatorConfig(1, 2, session);
			hostConfig.relayToOtherPeers = true;
			if (!r.host.Start(r.hostWire, hostConfig, &error) || !r.client.Start(r.clientWire, CoordinatorConfig(2, 1, session), &error)) return Fail(r, "coordinator start: " + error);
			if (!r.hostSession.StartHost(r.hostSessionWire, SessionConfig(static_cast<uint16_t>(r.port + 1), 0x5201, "Host"), &error) ||
			    !r.clientSession->StartClient(*r.clientSessionWire, "loopback", SessionConfig(static_cast<uint16_t>(r.port + 1), 0x5202, "Client"), &error)) {
				return Fail(r, "session start: " + error);
			}
			if (!Pump(r, 4000, [&r] { return r.host.IsRunning() && r.client.IsRunning() && r.hostSession.GetReadyPeerCount() == 1 && r.clientSession->IsReady() && r.hostSimulated >= 12 && r.clientSimulated >= 12; })) {
				return Fail(r, "the rig never ran twelve frames on both peers with both sessions ready");
			}
			// The match service defers every stop to its tick boundary once the round runs.
			r.host.DeferStopsToTickBoundary();
			r.client.DeferStopsToTickBoundary();
			return true;
		}

		NetPeerId HostSessionPeer(const Rig& r) {
			const auto peers = r.hostSession.GetReadyPeers();
			return peers.empty() ? c_InvalidNetPeerId : peers.front().transportPeerId;
		}

		bool ScheduleReclaim(Rig& r, std::string& error) {
			const auto& incarnations = r.host.GetConfig().peerIncarnations;
			const auto known = incarnations.find(2);
			const uint32_t incarnation = (known == incarnations.end() ? 1U : known->second) + 1U;
			const uint64_t frame = std::max(r.host.GetStats().nextFrame, r.host.SentInputThrough()) + 60;
			return r.host.SchedulePeerReclaim(2, 1, incarnation, frame, &error);
		}

		std::string SeatLabel(const Rig& r) {
			const NetLockstepCoordinator& host = r.host;
			const uint64_t frame = r.hostSimulated + 1;
			if (host.IsMigrating()) return "Migrating";
			if (host.HasAgreedSeatReclaim(2) && host.IsSeatUnderAI(2, frame)) return "Reclaiming";
			if (host.HasHeldAISeat(2) && !host.HasAgreedSeatReclaim(2)) return "Held";
			if (host.GetPeerLeaveFrames().contains(2) && !host.HasAgreedSeatReclaim(2)) return "Left";
			return "Active";
		}

		std::string Prefix(const std::string& reason) {
			const size_t colon = reason.find(':');
			return colon == std::string::npos ? reason : reason.substr(0, colon);
		}

		std::string CoordinatorLabel(const NetLockstepCoordinator& coordinator) {
			switch (coordinator.GetState()) {
				case NetLockstepState::Running: return "run";
				case NetLockstepState::Stopped: return coordinator.IsLocalSeatHeld() ? "held" : "stopped:" + Prefix(coordinator.GetStats().timeoutReason);
				case NetLockstepState::Failed: return "failed:" + Prefix(coordinator.GetStats().timeoutReason);
				case NetLockstepState::WaitingForStart: return "waiting";
				default: return "idle";
			}
		}

		bool SessionEnded(const NetSession& session) {
			const NetSessionState state = session.GetState();
			return state == NetSessionState::Closed || state == NetSessionState::Failed || state == NetSessionState::Rejected || state == NetSessionState::Stopped;
		}

		std::string SessionLabel(const NetSession& session) {
			if (SessionEnded(session)) return std::string("ended:") + (session.HasReject() ? NetProtocol::RejectReasonName(session.GetRejectReason()) : "none");
			return std::string(session.IsReady() ? "ready" : NetSession::StateName(session.GetState())) + "/" + NetSession::RejoinPhaseName(session.GetRejoinPhase());
		}

		struct Observation {
			std::string seat;
			std::string round;
			std::string peer;
			std::string session;
			bool sessionEnded = false;
			bool sessionReady = false;
			std::string sessionReason;
			std::string phase;
			uint32_t holds = 0;
			std::string api;

			std::string Describe() const {
				return "seat=" + seat + " round=" + round + " peer=" + peer + " sess=" + session + " holds=" + std::to_string(holds) + " api=" + api;
			}
		};

		Observation Observe(const Rig& r, const std::string& api) {
			Observation o;
			o.seat = SeatLabel(r);
			o.round = CoordinatorLabel(r.host);
			o.peer = CoordinatorLabel(r.client);
			o.session = SessionLabel(*r.clientSession);
			o.sessionEnded = SessionEnded(*r.clientSession);
			o.sessionReady = r.clientSession->IsReady();
			o.sessionReason = r.clientSession->HasReject() ? NetProtocol::RejectReasonName(r.clientSession->GetRejectReason()) : "none";
			o.phase = NetSession::RejoinPhaseName(r.clientSession->GetRejoinPhase());
			const uint32_t holds = HoldsOfSeat(r);
			o.holds = holds >= r.holdsAtEntry ? holds - r.holdsAtEntry : 0;
			o.api = api;
			return o;
		}

		bool TokenMatches(const std::string& key, const std::string& value, const Observation& o) {
			if (key == "seat") return o.seat == value;
			if (key == "round" || key == "peer") {
				const std::string& label = key == "round" ? o.round : o.peer;
				if (value == "run") return label == "run";
				if (value == "held") return label == "held";
				if (value == "over") return label == "stopped:Complete";
				if (value == "left") return label == "stopped:PeerLeft";
				if (value == "relaunch") return label == "failed:ResyncRequested";
				if (value == "ended") return label != "run";
				if (value == "noresync") return label != "failed:ResyncRequested" && label.find("ResyncRequested") == std::string::npos;
				return false;
			}
			if (key == "sess") {
				if (value == "ready") return o.sessionReady;
				if (value == "alive") return !o.sessionEnded;
				if (value == "ended") return o.sessionEnded;
				if (value.rfind("ended:", 0) == 0) return o.sessionEnded && o.sessionReason == value.substr(6);
				if (value.rfind("phase:", 0) == 0) return !o.sessionEnded && o.phase == value.substr(6);
				return false;
			}
			if (key == "holds") return value == "0" ? o.holds == 0 : value == ">0" ? o.holds > 0 : false;
			if (key == "api") return value == "ok" ? o.api == "ok" : value == "refused" ? o.api.rfind("refused", 0) == 0 : false;
			return false;
		}

		/// Every token of the expectation must hold; a token's value may list alternatives with '|'.
		bool Matches(const std::string& expect, const Observation& o, std::string& why) {
			std::istringstream tokens(expect);
			std::string token;
			bool all = true;
			while (tokens >> token) {
				std::string key, value;
				if (const size_t cut = token.find(">0"); cut != std::string::npos && token.find('=') == std::string::npos) {
					key = token.substr(0, cut);
					value = ">0";
				} else {
					const size_t equals = token.find('=');
					if (equals == std::string::npos) { why += " unparsed:" + token; all = false; continue; }
					key = token.substr(0, equals);
					value = token.substr(equals + 1);
				}
				bool any = false;
				std::istringstream choices(value);
				std::string choice;
				while (std::getline(choices, choice, '|')) any = any || TokenMatches(key, choice, o);
				if (!any) {
					why += " " + token;
					all = false;
				}
			}
			return all;
		}

		bool EnterActive(Rig& r) { return StartRig(r, 0x524A0000ULL + r.port); }

		bool EnterHeld(Rig& r) {
			if (!EnterActive(r)) return false;
			// The seat falls silent: the host holds it at the slow-player bound, then the seat hears the hold.
			r.clientLive = false;
			if (!Pump(r, 3000, [&r] { return r.host.HasHeldAISeat(2) && r.host.IsSeatUnderAI(2, r.hostSimulated + 1); })) return Fail(r, "the silent seat was never held");
			r.clientLive = true;
			r.clientFeeding = false;
			if (!Pump(r, 500, [&r] { return r.client.IsLocalSeatHeld(); })) return Fail(r, "the held seat never heard its hold");
			return true;
		}

		bool EnterReclaiming(Rig& r) {
			if (!EnterHeld(r)) return false;
			std::string error;
			if (!Pump(r, 2000, [&] { error.clear(); return r.host.PreparePeerRejoin(2, 10, r.now, &error); })) return Fail(r, "the held seat never became ready to rejoin: " + error);
			if (!ScheduleReclaim(r, error)) return Fail(r, "the host refused the held seat's reclaim: " + error);
			Pump(r, 20);
			if (SeatLabel(r) != "Reclaiming") return Fail(r, "the scheduled reclaim did not read as reclaiming: seat=" + SeatLabel(r));
			return true;
		}

		bool EnterRejoin(Rig& r, State state) {
			if (!EnterHeld(r)) return false;
			if (state == State::RejoinConnecting) {
				// A returning seat's fresh session handshakes live; the old one has gone.
				r.clientSession->Close("rejoin");
				Pump(r, 50, [&r] { return r.hostSession.GetReadyPeerCount() == 0; });
				r.clientSession.reset();
				r.clientSessionWire = std::make_unique<LoopbackTransport>();
				r.clientSession = std::make_unique<NetSession>();
				r.clientSession->SetRejoinPhase(NetSession::RejoinPhase::Connecting);
				std::string error;
				if (!r.clientSession->StartClient(*r.clientSessionWire, "loopback", SessionConfig(static_cast<uint16_t>(r.port + 1), 0x5203, "Client"), &error)) return Fail(r, "the returning session did not start: " + error);
				Step(r);
				if (r.clientSession->IsReady() || r.clientSession->GetRejoinPhase() != NetSession::RejoinPhase::Connecting) return Fail(r, "the returning session was not mid-handshake");
				return true;
			}
			if (state == State::RejoinImagePending) r.clientSession->EnterImagePending();
			else r.clientSession->SetRejoinPhase(state == State::RejoinLoading ? NetSession::RejoinPhase::Loading : NetSession::RejoinPhase::TailReplay);
			return true;
		}

		bool EnterParked(Rig& r) {
			if (!EnterActive(r)) return false;
			// A first capture sets the park's budget the way a real autosave does; the second one is the park under test.
			uint64_t completed = std::min(r.hostSimulated, r.clientSimulated);
			r.host.BeginSynchronizedCapture(completed);
			r.client.BeginSynchronizedCapture(completed);
			Pump(r, 10);
			r.host.CompleteSynchronizedCapture(completed, 120.0);
			r.client.CompleteSynchronizedCapture(completed, 120.0);
			if (!Pump(r, 3000, [&r, completed] { return r.hostSimulated > completed + 4 && !r.host.IsSynchronizedCapturePark(r.hostSimulated + 1) && r.clientSimulated >= r.hostSimulated; })) {
				return Fail(r, "the warm-up park never closed");
			}
			completed = std::min(r.hostSimulated, r.clientSimulated);
			r.host.BeginSynchronizedCapture(completed);
			r.client.BeginSynchronizedCapture(completed);
			r.parkCompleted = completed;
			r.autoCompleteAtMs = r.now + 60;
			if (!Pump(r, 1000, [&r] { return r.host.IsSynchronizedCapturePark(r.hostSimulated + 1); })) return Fail(r, "the round never reached its park");
			return true;
		}

		bool EnterRelaunching(Rig& r) {
			if (!EnterActive(r)) return false;
			r.host.RequestResync("rejoin matrix relaunch");
			Pump(r, 100, [&r] { return !r.client.IsRunning(); });
			if (CoordinatorLabel(r.host) != "failed:ResyncRequested") return Fail(r, "the host did not relaunch");
			return true;
		}

		bool EnterDraining(Rig& r) {
			if (!EnterActive(r)) return false;
			r.finalFrame = r.hostSimulated + 3;
			r.host.SetFinalFrame(r.finalFrame);
			r.client.SetFinalFrame(r.finalFrame);
			if (!Pump(r, 1000, [&r] { return r.hostSimulated >= r.finalFrame && r.clientSimulated >= r.finalFrame; })) return Fail(r, "the round never reached its last tick");
			r.host.SetGoodbyeDrain(true);
			r.client.SetGoodbyeDrain(true);
			if (!r.host.IsRunning()) return Fail(r, "the host stopped before its goodbye drain");
			return true;
		}

		bool EnterLeft(Rig& r) {
			if (!EnterActive(r)) return false;
			r.client.Leave("rejoin matrix leave");
			r.clientFeeding = false;
			Pump(r, 300, [&r] { return r.host.HasHeldAISeat(2) || r.host.GetPeerLeaveFrames().contains(2); });
			if (CoordinatorLabel(r.client) != "stopped:PeerLeft") return Fail(r, "the client did not leave");
			if (!r.host.HasHeldAISeat(2) && !r.host.GetPeerLeaveFrames().contains(2)) return Fail(r, "the host never recorded the leave");
			return true;
		}

		bool Enter(Rig& r, State state) {
			switch (state) {
				case State::Active: return EnterActive(r);
				case State::Held: return EnterHeld(r);
				case State::Reclaiming: return EnterReclaiming(r);
				case State::Parked: return EnterParked(r);
				case State::Relaunching: return EnterRelaunching(r);
				case State::Draining: return EnterDraining(r);
				case State::Left: return EnterLeft(r);
				default: return IsRejoin(state) ? EnterRejoin(r, state) : Fail(r, "no entry for this state");
			}
		}

		std::string Apply(Rig& r, Event event) {
			std::string error;
			switch (event) {
				case Event::HoldProposed: return r.host.ProposePeerHold(2, r.now, &error) ? "ok" : "refused:" + error;
				case Event::HoldResolved: r.host.ResolveHeldSeat(2, NetLockstepHoldResolution::Reclaimed, r.now); return "ok";
				case Event::ParkBegin: {
					const uint64_t completed = r.client.IsRunning() ? std::min(r.hostSimulated, r.clientSimulated) : r.hostSimulated;
					r.host.BeginSynchronizedCapture(completed);
					r.client.BeginSynchronizedCapture(completed);
					r.parkCompleted = completed;
					r.autoCompleteAtMs = r.now + 60;
					return "ok";
				}
				case Event::ParkEnd: {
					const uint64_t completed = r.parkCompleted != UINT64_MAX ? r.parkCompleted : r.hostSimulated;
					r.host.CompleteSynchronizedCapture(completed, 30.0);
					r.client.CompleteSynchronizedCapture(completed, 30.0);
					r.autoCompleteAtMs = UINT64_MAX;
					return "ok";
				}
				case Event::ResyncRelaunch: r.host.RequestResync("rejoin matrix relaunch"); return "ok";
				case Event::HostGoodbye:
					r.host.Leave("The host left the match");
					r.hostSession.EndHostedSession("The host left the match");
					return "ok";
				case Event::HostLost: {
					// The host's process is gone: nothing more comes from it, and each of the seat's links reports its close.
					r.hostLive = false;
					r.client.InjectEvent({NetTransportEventType::PeerDisconnected, 1, NetTransportLane::ControlReliable, {}, "Connection dropped"}, r.now);
					r.clientSession->InjectEvent({NetTransportEventType::PeerDisconnected, r.clientSession->GetRemoteTransportPeerId(), NetTransportLane::ControlReliable, {}, "Connection dropped"}, r.now);
					return "ok";
				}
				case Event::MigrationBegin: return r.client.BeginHostMigration(r.now) ? "ok" : "refused";
				case Event::LateJoin: {
					NetGameWorldTransition transition;
					transition.kind = NetGameWorldTransition::Activate;
					transition.peerId = 3;
					transition.holderGeneration = 1;
					transition.activationFrame = r.host.GetStats().nextFrame + 40;
					return r.host.ProposeWorldAdmission(77, 1, transition, &error) ? "ok" : "refused:" + error;
				}
				case Event::TicketRejoin: r.host.InjectEvent({NetTransportEventType::PeerConnected, 77, NetTransportLane::ControlReliable, {}, {}}, r.now); return "ok";
				case Event::HeldRejoin:
					if (!Pump(r, 2000, [&] { error.clear(); return r.host.PreparePeerRejoin(2, 10, r.now, &error); })) return "refused:" + error;
					return ScheduleReclaim(r, error) ? "ok" : "refused:" + error;
				case Event::Kick:
				case Event::Ban: {
					const bool kick = event == Event::Kick;
					const NetPeerId peer = HostSessionPeer(r);
					r.host.EvictRemovedPeer(2, kick ? "kicked" : "banned", r.now);
					if (peer != c_InvalidNetPeerId) r.hostSession.DisconnectReadyPeer(peer, kick ? NetRejectReason::ParticipantRemoved : NetRejectReason::ParticipantBanned, kick ? "kicked" : "banned");
					return "ok";
				}
				case Event::SeatRelease: r.host.ResolveHeldSeat(2, NetLockstepHoldResolution::Expired, r.now); return "ok";
				case Event::OwnCap:
					r.client.Complete("e2e complete");
					r.clientSession->Close("own cap reached");
					return "ok";
				case Event::MatchOver: {
					const NetPeerId peer = HostSessionPeer(r);
					const std::string goodbye = "match over through frame " + std::to_string(r.hostSimulated);
					r.host.Complete("match over");
					r.hostSession.DisconnectJoiningPeers(NetRejectReason::SessionEnded, goodbye);
					if (peer != c_InvalidNetPeerId) r.hostSession.DisconnectReadyPeer(peer, NetRejectReason::SessionEnded, goodbye);
					return "ok";
				}
				case Event::LinkBlip:
					// Thirty milliseconds of silence: under the fifty-millisecond slow-player bound and far under the transport timeout.
					r.clientLive = false;
					Pump(r, 30);
					r.clientLive = true;
					return "ok";
				case Event::LinkRestore:
					r.clientLive = true;
					r.clientFeeding = true;
					return "ok";
				default: return "none";
			}
		}

		struct Totals {
			int pairs = 0;
			int tierOne = 0;
			int tierTwo = 0;
			int walked = 0;
			int walkedTierOne = 0;
			int walkedTierTwo = 0;
			int pass = 0;
			int fail = 0;
			int entryFail = 0;
			int gaps = 0;
			int notWalked = 0;
		};
	} // namespace

	int NetRejoinMatrixSelfTest::Run() {
		Totals totals;
		std::vector<std::pair<State, Event>> pairs;
		for (uint8_t s = 0; s < static_cast<uint8_t>(State::Count); ++s) {
			for (uint8_t e = 0; e < static_cast<uint8_t>(Event::Count); ++e) pairs.emplace_back(static_cast<State>(s), static_cast<Event>(e));
		}
		// The table first, whole, so the document is written from the same rows the walk reads.
		for (const auto& [state, event]: pairs) {
			const Expectation x = Expect(state, event);
			std::cout << c_Tag << " table " << StateName(state) << " | " << EventName(event) << " | tier " << Tier(state, event) << " | " << x.expect << " | " << x.source << " | "
			          << (x.gap.empty() ? "-" : x.gap) << " | " << (x.notWalked.empty() ? (x.note.empty() ? std::string("walked") : "walked; " + x.note) : "not walked: " + x.notWalked) << std::endl;
		}
		uint16_t port = 52000;
		for (const auto& [state, event]: pairs) {
			const Expectation x = Expect(state, event);
			const int tier = Tier(state, event);
			++totals.pairs;
			(tier == 1 ? totals.tierOne : totals.tierTwo) += 1;
			if (!x.gap.empty()) ++totals.gaps;
			const std::string name = std::string(StateName(state)) + " x " + EventName(event);
			if (!x.notWalked.empty()) {
				++totals.notWalked;
				std::cout << c_Tag << " pair " << name << " tier=" << tier << " result=NOT-WALKED reason=\"" << x.notWalked << "\"" << std::endl;
				continue;
			}
			auto rig = std::make_unique<Rig>();
			rig->port = port;
			port = static_cast<uint16_t>(port + 2);
			if (!Enter(*rig, state)) {
				++totals.entryFail;
				std::cout << c_Tag << " pair " << name << " tier=" << tier << " result=ENTRY-FAIL expected=\"" << x.expect << "\" error=\"" << rig->error << "\"" << std::endl;
				continue;
			}
			rig->holdsAtEntry = HoldsOfSeat(*rig);
			const std::string api = Apply(*rig, event);
			Pump(*rig, 500);
			const Observation observed = Observe(*rig, api);
			std::string why;
			const bool pass = Matches(x.expect, observed, why);
			++totals.walked;
			(tier == 1 ? totals.walkedTierOne : totals.walkedTierTwo) += 1;
			(pass ? totals.pass : totals.fail) += 1;
			std::cout << c_Tag << " pair " << name << " tier=" << tier << " result=" << (pass ? "PASS" : "FAIL") << " expected=\"" << x.expect << "\" actual=\"" << observed.Describe()
			          << "\"" << (pass ? "" : " unmet=\"" + why.substr(why.empty() ? 0 : 1) + "\"") << " source=" << x.source << (x.gap.empty() ? "" : " gap=yes") << std::endl;
		}
		std::cout << c_Tag << " totals pairs=" << totals.pairs << " tier1=" << totals.tierOne << " tier2=" << totals.tierTwo << " walked=" << totals.walked
		          << " walked_tier1=" << totals.walkedTierOne << " walked_tier2=" << totals.walkedTierTwo << " pass=" << totals.pass << " fail=" << totals.fail
		          << " entry_fail=" << totals.entryFail << " gaps=" << totals.gaps << " not_walked=" << totals.notWalked << std::endl;
		if (totals.fail != 0 || totals.entryFail != 0) {
			std::cout << c_Tag << " FAIL pairs_red=" << totals.fail << " entry_red=" << totals.entryFail << std::endl;
			return 1;
		}
		std::cout << c_Tag << " PASS" << std::endl;
		return 0;
	}

} // namespace RTE
