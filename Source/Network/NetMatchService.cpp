#include "NetMatchService.h"
#include "NetA7Journal.h"

#include "ActivityMan.h"
#include "Constants.h"
#include "GameActivity.h"
#include "FrameMan.h"
#include "GUIInput.h"
#include "GnsTransport.h"
#include "NetIdentity.h"
#include "NetProtocol.h"
#include "PresetMan.h"
#include "ScenarioRunner.h"
#include "System.h"
#include "System/FaultInjection.h"
#include "TimerMan.h"
#include "UInputMan.h"

#include "MovableMan.h"

#include "nlohmann/json.hpp"

#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <utility>

namespace RTE {

	bool NetMatchService::s_AdmissionEnabled = true;
	std::string NetMatchService::s_TicketStorePath;
	std::string NetMatchService::s_JoinWaitPath;
	bool NetMatchService::s_ApplyForSeat = false;
	uint16_t NetMatchService::s_ApplySeat = 0;
	bool NetMatchService::s_AutoSubstitute = false;
	uint16_t NetMatchService::s_AutoSubstituteSeat = 0;
	uint64_t NetMatchService::s_AutoSubstituteDelayMs = 0;
	bool NetMatchService::s_AutoSubstituteThenCancel = false;

// Per-process file names: same-machine instances share Userdata, so concurrent matches must never share a snapshot file.
static std::string ResyncSaveName() {
	return "p5resync_" + std::to_string(System::GetProcessID());
}

	namespace {
		using json = nlohmann::json;

		constexpr uint64_t c_UiSessionId = 0x5354414745325034ULL;
		constexpr uint64_t c_HostNonce = 0x503441484F53544ULL;
		constexpr uint64_t c_ClientNonce = 0x503441434C49454ULL;
		constexpr uint32_t c_MenuLobbyWaitMs = 10 * 60 * 1000;

		// The host dedupes joiners by nonce, so two clients on one session must present distinct ones.
		// Transport-level identity only — the sim never sees it, so real randomness is fine here.
		json AdmissionJsonFromHost(const NetReconnectHost& host) {
			const NetReconnectHostStats& stats = host.GetStats();
			return json{
				{"new_joins", stats.newJoins},
				{"ticket_offers_sent", stats.ticketOffersSent},
				{"ticket_offer_retransmits", stats.ticketOfferRetransmits},
				{"provisional_seats_opened", stats.provisionalSeatsOpened},
				{"provisional_seats_committed", stats.provisionalSeatsCommitted},
				{"provisional_seats_expired", stats.provisionalSeatsExpired},
				{"provisional_seats_refused", stats.provisionalSeatsRefused},
				{"provisional_seats_resumed", stats.provisionalSeatsResumed},
				{"persistence_failures", stats.persistenceFailures},
				{"reclaims_accepted", stats.reclaimsAccepted},
				{"identity_rejections", stats.identityRejections},
				{"denials_scheduled", stats.denialsScheduled},
				{"denials_released", stats.denialsReleased},
				{"replayed_results", stats.replayedResults},
				{"stale_epoch_drops", stats.staleEpochDrops},
				{"unknown_transaction_drops", stats.unknownTransactionDrops},
				{"fenced_packets", stats.fencedPackets},
				{"fenced_disconnects", stats.fencedDisconnects},
				{"incarnations_bound", stats.incarnationsBound},
				{"seats_dropped", stats.seatsDropped},
				{"seats_closed_by_leave", stats.seatsClosedByLeave},
				{"ledger_drops_recorded", stats.ledgerDropsRecorded},
				{"reseats_issued", stats.reseatsIssued},
				{"reseats_without_a_ledger", stats.reseatsWithoutALedger},
				{"reseats_without_survivors", stats.reseatsWithoutSurvivors},
				{"reseat_live_on_team_not_named", stats.reseatLiveOnTeamNotNamed},
				{"reclaim_retransmits_dropped", stats.reclaimRetransmitsDropped},
				{"seat_holds_expired", stats.seatHoldsExpired},
				{"seats_released_in_lobby", stats.seatsReleasedInLobby},
				{"applicants_registered", stats.applicantsRegistered},
				{"applicants_refused", stats.applicantsRefused},
				{"applicants_expired", stats.applicantsExpired},
				{"applicants_displaced", stats.applicantsDisplaced},
				{"substitution_offers_sent", stats.substitutionOffersSent},
				{"substitution_offer_retransmits", stats.substitutionOfferRetransmits},
				{"substitutions_committed", stats.substitutionsCommitted},
				{"substitutions_cancelled", stats.substitutionsCancelled},
				{"substitutions_superseded", stats.substitutionsSuperseded},
				{"substitution_ack_failures", stats.substitutionAckFailures},
				{"reassigned_reclaims_refused", stats.reassignedReclaimsRefused},
				{"pending_applicants", 0},
			};
		}

		uint64_t MakeClientNonce() {
			std::random_device device;
			return c_ClientNonce ^ (static_cast<uint64_t>(device()) << 32) ^ static_cast<uint64_t>(device());
		}

		// Set only while PumpSessionEvents runs, which is the game thread inside the sim tick. The
		// ownership census walks g_MovableMan, so any other thread asking for one is refused.
		thread_local bool t_SimCensusOpen = false;

		struct SimCensusScope {
			SimCensusScope() { t_SimCensusOpen = true; }
			~SimCensusScope() { t_SimCensusOpen = false; }
		};

		uint64_t SteadyNowMs() {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		uint64_t UnixNowMs(void*) {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
		}

		NetH4Identity BuildH4Identity(const NetIdentityManifest& manifest) {
			NetH4Identity identity;
			identity.controllerFrameVersion = manifest.controllerFrameVersion;
			identity.controllerFrameEncodedSize = manifest.controllerFrameEncodedSize;
			identity.gameVersion = manifest.gameVersion;
			identity.buildId = manifest.buildId;
			identity.deterministicConfigHash = manifest.deterministicConfigHash;
			identity.moduleManifestHash = manifest.moduleManifestHash;
			identity.sessionRulesHash = manifest.sessionRulesHash;
			identity.sessionIdentityHash = manifest.sessionIdentityHash;
			return identity;
		}

		std::string PlayerNameOrDefault(const NetMatchServiceRequest& request, bool hostSlot) {
			const bool localSlot = request.host == hostSlot;
			if (localSlot && !request.playerName.empty()) {
				return request.playerName;
			}
			return hostSlot ? "Host" : "Client";
		}
	}

	NetMatchService::~NetMatchService() {
		Destroy();
	}

	bool NetMatchService::Start(const NetMatchServiceRequest& request, std::string* error) {
		Destroy();
		m_CancelRequested.store(false);
		m_ReadyRequested.store(false);
		m_StartRequested.store(false);
		if (request.port == 0) {
			if (error) *error = "port must be nonzero";
			return false;
		}
		if (!request.host && request.address.empty()) {
			if (error) *error = "join address must not be empty";
			return false;
		}

		g_TimerMan.SetDeltaTimeSecs(c_DefaultDeltaTimeS);
		NetIdentityManifest manifest;
		NetIdentityBuildOptions identityOptions;
		identityOptions.buildId = "stage2-p2d-local";
		identityOptions.sessionRulesTag = "stage2-p2-session-rules";
		std::string buildError;
		if (!NetIdentity::BuildCurrentManifest(manifest, &buildError, identityOptions)) {
			if (error) *error = buildError;
			SetState(NetMatchServiceState::Failed, "Identity build failed", buildError);
			return false;
		}

		if (!request.host && NetA7Journal::HasConnectGate() && !WaitForA7ConnectGate(error)) return false;

		// Startup is done and nothing is connected yet, so this is where a held joiner waits.
		if (!request.host && !WaitForJoinTrigger(s_JoinWaitPath, c_JoinWaitBudgetMs, error)) return false;

		m_ActivityPreset = request.activityPreset;
		SetState(NetMatchServiceState::Starting, request.host ? "Hosting direct-IP match" : "Joining direct-IP match");
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_WorkerDone = false;
			m_IsHost = request.host;
			m_LocalPeerId = request.host ? 1 : 2;
			m_LocalTeam = request.host ? 0 : 1;
			m_InputDelayText.clear();
			m_ResyncOnDesync = request.resyncOnDesync;
			m_PendingResyncLoad.clear();
			m_BeaconGamePort = request.port;
			m_BeaconMaxPlayers = request.peerCount;
			m_LocalName = request.playerName.empty() ? (request.host ? "Host" : "Client") : request.playerName;
		}
		m_EverStarted.store(true);
		m_Worker = std::thread(&NetMatchService::WorkerMain, this, request, std::move(manifest));
		return true;
	}

	bool NetMatchService::ReturnToLobby(std::string* error) {
		JoinWorkerIfDone();
		std::unique_ptr<GnsTransport> transport;
		std::unique_ptr<NetSession> session;
		std::unique_ptr<NetLockstepCoordinator> coordinator;
		std::unique_ptr<NetMatchRunner> runner;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_State != NetMatchServiceState::Completed || !m_Transport || !m_Session || !m_Runner) {
				if (error) *error = "no completed match to rematch";
				return false;
			}
			if (!m_Session->IsReady()) {
				// Session lost (the other player quit); settle so the UI stops offering a rematch.
				m_State = NetMatchServiceState::Failed;
				m_StatusText = "Rematch unavailable";
				m_ErrorText = m_Session->HasReject() ? m_Session->BuildRejectText() : "the other player left";
				if (error) *error = m_ErrorText;
				return false;
			}
			m_PendingResyncState.reset();
			m_ResyncRetainsLocalState = false;
			m_ResyncSourceRound = 0;
			transport = std::move(m_Transport);
			session = std::move(m_Session);
			runner = std::move(m_Runner);
			if (m_IsHost) {
				// A rematch restarts from a zeroed sim count.
				runner->SetStartFrame(1);
			}
			m_Coordinator.reset();
			coordinator = std::make_unique<NetLockstepCoordinator>();
			m_WorkerDone = false;
			m_State = NetMatchServiceState::Starting;
			m_StatusText += " - ready up for a rematch";
			m_ErrorText.clear();
		}
		m_CancelRequested.store(false);
		m_ReadyRequested.store(false);
		m_StartRequested.store(false);
		m_Worker = std::thread(&NetMatchService::WorkerRematchMain, this, transport.release(), session.release(), coordinator.release(), runner.release());
		return true;
	}

	bool NetMatchService::ResyncSnapshotAllowed(const Activity* activity) {
		return activity != nullptr && activity->GetActivityState() != Activity::Over;
	}

	NetRejoinAnswer NetMatchService::ClassifyRejoin(const Activity* activity) {
		return ResyncSnapshotAllowed(activity) ? NetRejoinAnswer::Resync : NetRejoinAnswer::MatchOver;
	}

	void NetMatchService::AnswerMatchOverRejoin(const std::string& result) {
		const std::string text = result.empty() ? "match over" : result;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_RejoinOutcome = "match_over";
			if (m_Transport && m_Session && m_Coordinator) {
				for (const NetSessionPeerInfo& peer: m_Session->GetReadyPeers()) {
					if (!m_Coordinator->UsesTransportPeer(peer.transportPeerId)) {
						NetMessage message;
						message.payload = NetDisconnect{static_cast<uint16_t>(NetRejectReason::SessionEnded), text};
						std::vector<uint8_t> bytes;
						if (NetProtocol::Encode(message, bytes)) {
							m_Transport->Send(peer.transportPeerId, NetTransportLane::ControlReliable, bytes, nullptr);
							m_Transport->Disconnect(peer.transportPeerId, text);
						}
					}
				}
			}
		}
		// The session pump must not destroy the coordinator; the main loop FinishMatch's the stop.
		Complete(text);
	}

	bool NetMatchService::ResyncMatch(std::string* error) {
		JoinWorkerIfDone();
		// The host snapshots the live (diverged) match BEFORE the teardown; both sides then reload
		// the identical file, so the divergence is healed by construction.
		std::vector<uint8_t> stateBytes;
		bool isHost = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			isHost = m_IsHost;
		}
		const uint64_t a7Resync = NetA7Journal::BeginResync();
		const bool a7Save = NetA7Journal::Enabled() && isHost && FaultInjected("slow_resync_save");
		const uint64_t a7SaveBegin = a7Save ? AdmissionNowMs() : 0;
		if (a7Save) NetA7Journal::Session("save_begin", a7SaveBegin, {{"resync", std::to_string(a7Resync)}}, "NetMatchService::AdmissionNowMs");
		if (isHost && ClassifyRejoin(g_ActivityMan.GetActivity()) == NetRejoinAnswer::MatchOver) {
			AnswerMatchOverRejoin("match over");
			if (error) {
				*error = "match over";
			}
			return false;
		}
		uint64_t dropFrame = 0;
		if (isHost) {
			// Snapshot callbacks stay on the game thread while waiting peers keep hearing from us.
			std::jthread keepalive([this](std::stop_token stop) {
				while (!stop.stop_requested()) {
					{
						std::lock_guard<std::mutex> lock(m_Mutex);
						if (m_Session) m_Session->TickKeepalive(AdmissionNowMs());
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
				}
			});
			// The healed round resumes at the first frame the sim has not applied.
			bool rewindSim = false;
			if (!ScenarioRunner::ResolveResyncDropFrame(ScenarioRunner::GetLockstepResumeFrame(), static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()), dropFrame, rewindSim, error)) {
				return false;
			}
			if (rewindSim) {
				long long time = g_TimerMan.GetSimTimeTicks();
				if (!g_TimerMan.IsSimTimeFrozen()) {
					const long long delta = g_TimerMan.GetDeltaTimeTicks();
					if (time >= delta) {
						time -= delta;
					}
				}
				g_TimerMan.RewindSimTo(static_cast<long long>(dropFrame - 1), time);
			}
			if (!g_ActivityMan.SaveCurrentGame(ResyncSaveName()) || !g_ActivityMan.WaitForSaveGameTask()) {
				if (error) *error = "resync snapshot save failed";
				return false;
			}
			if (FaultInjected("slow_resync_save")) {
				// Keep the snapshot frozen across a save longer than the receive timeout.
				std::this_thread::sleep_for(std::chrono::seconds(7));
			}
			if (a7Save) {
				const uint64_t ended = AdmissionNowMs();
				NetA7Journal::Session("save_end", ended, {{"resync", std::to_string(a7Resync)}, {"fault", "slow_resync_save"}, {"elapsed_ms", ended - a7SaveBegin}}, "NetMatchService::AdmissionNowMs");
			}
			const std::string savePath = g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/" + ResyncSaveName() + ".ccsave";
			std::ifstream in(savePath, std::ios::binary);
			if (!in) {
				if (error) *error = "resync snapshot is unreadable: " + savePath;
				return false;
			}
			stateBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			if (stateBytes.empty()) {
				if (error) *error = "resync snapshot is empty";
				return false;
			}
			NetResyncState state;
			std::vector<uint8_t> envelope;
			if (!ScenarioRunner::CaptureNetResyncState(dropFrame > 0 ? dropFrame - 1 : 0, state, error) || !NetResyncCodec::Encode(state, stateBytes, envelope, error)) return false;
			stateBytes = std::move(envelope);
		}
		m_ResyncSourceRound = ScenarioRunner::GetLockstepRoundId();
		m_ResyncRetainsLocalState = true;
		ScenarioRunner::SetLockstepCoordinator(nullptr);
		ScenarioRunner::SetSessionPump(nullptr);
		std::unique_ptr<GnsTransport> transport;
		std::unique_ptr<NetSession> session;
		std::unique_ptr<NetLockstepCoordinator> coordinator;
		std::unique_ptr<NetMatchRunner> runner;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (!m_Transport || !m_Session || !m_Runner) {
				if (error) *error = "no live match to resync";
				return false;
			}
			if (!m_Session->IsReady()) {
				m_State = NetMatchServiceState::Failed;
				m_StatusText = "Resync unavailable";
				m_ErrorText = m_Session->HasReject() ? m_Session->BuildRejectText() : "the session was lost";
				if (error) *error = m_ErrorText;
				return false;
			}
			transport = std::move(m_Transport);
			session = std::move(m_Session);
			runner = std::move(m_Runner);
			if (isHost) {
				runner->SetStartFrame(ScenarioRunner::ResyncResumeStartFrame(dropFrame));
			}
			m_Coordinator.reset();
			coordinator = std::make_unique<NetLockstepCoordinator>();
			m_WorkerDone = false;
			m_State = NetMatchServiceState::Starting;
			m_StatusText = "Resyncing the match";
			m_ErrorText.clear();
			m_PendingResyncLoad.clear();
		}
		m_CancelRequested.store(false);
		m_ReadyRequested.store(true);
		m_StartRequested.store(isHost);
		m_Worker = std::thread(&NetMatchService::WorkerResyncMain, this, transport.release(), session.release(), coordinator.release(), runner.release(), std::move(stateBytes));
		return true;
	}

	void NetMatchService::WorkerResyncMain(GnsTransport* transportRaw, NetSession* sessionRaw, NetLockstepCoordinator* coordinatorRaw, NetMatchRunner* runnerRaw, std::vector<uint8_t> stateBytes) {
		std::unique_ptr<GnsTransport> transport(transportRaw);
		std::unique_ptr<NetSession> session(sessionRaw);
		std::unique_ptr<NetLockstepCoordinator> coordinator(coordinatorRaw);
		std::unique_ptr<NetMatchRunner> runner(runnerRaw);
		std::string error;
		const bool started = runner->StartNextMatch(*transport, *session, *coordinator, &error, stateBytes);
		std::string pendingLoad;
		NetResyncState resyncState;
		if (started) {
			std::vector<uint8_t> receivedState = runner->TakeReceivedState();
			if (receivedState.empty()) receivedState = std::move(stateBytes);
			(void)PrepareReceivedResync(receivedState, *coordinator, pendingLoad, resyncState, &error);
		}
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Transport = std::move(transport);
			m_Session = std::move(session);
			m_Coordinator = std::move(coordinator);
			m_Runner = std::move(runner);
			if (started && !pendingLoad.empty()) {
				m_PendingResyncLoad = pendingLoad;
				m_PendingResyncState = std::move(resyncState);
				m_State = NetMatchServiceState::ReadyToLaunch;
				m_StatusText = "Resynced; relaunching match";
				m_ErrorText.clear();
			} else {
				m_State = NetMatchServiceState::Failed;
				m_StatusText = "Resync failed";
				m_ErrorText = error;
			}
			m_WorkerDone = true;
		}
	}

	bool NetMatchService::PrepareReceivedResync(const std::vector<uint8_t>& bytes, const NetLockstepCoordinator& coordinator, std::string& pendingLoad, NetResyncState& state, std::string* error) {
		std::vector<uint8_t> archive;
		if (!NetResyncCodec::Decode(bytes, coordinator.GetConfig().sessionId, coordinator.GetConfig().startFrame, state, archive, error)) return false;
		if ((m_ResyncSourceRound != 0 && state.sourceRound != m_ResyncSourceRound) || state.sourceRound == coordinator.GetRoundId()) {
			if (error) *error = "resync snapshot round does not match the ended match";
			return false;
		}
		const auto name = ResyncSaveName() + "_recv";
		const auto path = g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/" + name + ".ccsave";
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size()));
		out.close();
		if (!out.good()) { if (error) *error = "could not write the received resync snapshot"; return false; }
		pendingLoad = name;
		return true;
	}

	std::string NetMatchService::TakePendingResyncLoad() {
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::string pending = std::move(m_PendingResyncLoad);
		m_PendingResyncLoad.clear();
		return pending;
	}

	bool NetMatchService::HasPendingResyncLoad() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return !m_PendingResyncLoad.empty();
	}

	bool NetMatchService::StageResyncedMatchLaunch(std::string* error) {
		const std::string pendingLoad = TakePendingResyncLoad();
		if (pendingLoad.empty() || !m_PendingResyncState) {
			if (error) *error = "no resync snapshot to load";
			return false;
		}
		const auto state = std::make_shared<NetResyncState>(*m_PendingResyncState);
		const uint8_t localPeer = GetLocalPeerId();
		const bool retainLocal = m_ResyncRetainsLocalState;
		std::optional<NetResyncPlayerBindings> newestBinding;
		if (const auto found = state->playerBindings.find(localPeer); found != state->playerBindings.end()) newestBinding = found->second;
		for (const auto& pending: state->pendingPlayerBindings) if (pending.command.senderPeerId == localPeer && (!newestBinding || pending.frame > newestBinding->frame)) {
			newestBinding = NetResyncPlayerBindings{pending.frame, std::get<NetGamePlayerBindings>(pending.command.payload)};
		}
		if (!retainLocal && !newestBinding) {
			if (error) *error = "the resync snapshot has no player bindings for this peer";
			return false;
		}
		if (!g_ActivityMan.LoadGameToRestart(pendingLoad)) {
			if (error) *error = "resync snapshot load failed: " + pendingLoad;
			return false;
		}
		const char* keepResyncSaves = std::getenv("CC_KEEP_RESYNC_SAVES");
		if (keepResyncSaves && keepResyncSaves[0] && keepResyncSaves[0] != '0') {
			std::cout << "[net-match] keeping resync save: " << pendingLoad << std::endl;
		} else {
			g_ActivityMan.RemoveSavedGame(pendingLoad);
		}
		std::cout << "[net-match] launching from the received snapshot: " << pendingLoad << std::endl;
		struct LocalState { Activity::NetLocalPlayerState activity; std::string input, gui, frame; };
		const auto local = std::make_shared<LocalState>();
		if (!g_ActivityMan.SetPendingCheckpointCallbacks([local, retainLocal] {
			local->input = g_UInputMan.SaveCheckpoint();
			local->gui = GUIInput::SaveSharedCheckpoint();
			local->frame = g_FrameMan.SaveNetLocalState();
			return !retainLocal || (g_ActivityMan.GetActivity() && g_ActivityMan.GetActivity()->CaptureNetLocalPlayerState(local->activity));
		}, [local, state, retainLocal, newestBinding](Activity& activity) {
			if (static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) != state->savedTick ||
			    !g_UInputMan.LoadCheckpoint(local->input, true) || !GUIInput::LoadSharedCheckpoint(local->gui, true) || !g_FrameMan.LoadNetLocalState(local->frame, true)) return false;
			if (!(retainLocal ? activity.RestoreNetLocalPlayerState(local->activity) : activity.ApplyNetPlayerBindings(newestBinding->bindings))) return false;
			if (!g_UInputMan.LoadCheckpoint(local->input) || !GUIInput::LoadSharedCheckpoint(local->gui) || !g_FrameMan.LoadNetLocalState(local->frame)) return false;
			return ScenarioRunner::RestoreNetResyncState(*state);
		})) { if (error) *error = "could not stage resync local state restoration"; return false; }
		ScenarioRunner::ApplyDeterministicConfig();
		return true;
	}

	// Same shape as WorkerMain: the objects live as worker locals while the lobby round runs, so
	// report/snapshot readers never race a mid-mutation runner; they move back in when it settles.
	void NetMatchService::WorkerRematchMain(GnsTransport* transportRaw, NetSession* sessionRaw, NetLockstepCoordinator* coordinatorRaw, NetMatchRunner* runnerRaw) {
		std::unique_ptr<GnsTransport> transport(transportRaw);
		std::unique_ptr<NetSession> session(sessionRaw);
		std::unique_ptr<NetLockstepCoordinator> coordinator(coordinatorRaw);
		std::unique_ptr<NetMatchRunner> runner(runnerRaw);
		std::string error;
		const bool started = runner->StartNextMatch(*transport, *session, *coordinator, &error);
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Transport = std::move(transport);
			m_Session = std::move(session);
			m_Coordinator = std::move(coordinator);
			m_Runner = std::move(runner);
			if (started) {
				m_State = NetMatchServiceState::ReadyToLaunch;
				m_StatusText = "Ready to launch match";
				m_ErrorText.clear();
			} else {
				m_State = NetMatchServiceState::Failed;
				m_StatusText = "Rematch setup failed";
				m_ErrorText = error;
			}
			m_WorkerDone = true;
		}
	}

	void NetMatchService::ResetRosterTransitionHistory() {
		m_RosterTransitions.clear();
		m_RosterTransitionsDropped = 0;
		m_LastRosterPair.clear();
	}

	void NetMatchService::RecordRosterTransitions(uint64_t observedAtMs) {
		const uint64_t appliedFrame = ScenarioRunner::GetLockstepAppliedFrame();
		for (const NetLobbyMember& member: m_LobbySnapshot.members) {
			const std::string state = NetSeatPresence::StateName(m_SeatPresence.StateOf(member.peerId));
			const std::string line = m_SeatPresence.Line(member.peerId, member.displayName);
			std::pair<std::string, std::string>& last = m_LastRosterPair[member.peerId];
			if (last.first == state && last.second == line) {
				continue;
			}
			const std::string previous = last.first;
			last = {state, line};
			if (!previous.empty()) {
				const std::string who = member.displayName.empty() ? "Player " + std::to_string(member.peerId) : member.displayName;
				const bool wasAway = previous == "Disconnected" || previous == "Reconnecting" || previous == "Left";
				if ((state == "Disconnected" || state == "Left") && !wasAway) {
					ScenarioRunner::PushNetUiToast("player_left", "Player " + who + " left");
				} else if (state == "Present" && wasAway) {
					ScenarioRunner::PushNetUiToast("player_rejoined", "Player " + who + " rejoined");
				}
			}
			if (m_RosterTransitions.size() >= 256) {
				++m_RosterTransitionsDropped;
				continue;
			}
			m_RosterTransitions.push_back({member.peerId, state, line, appliedFrame, observedAtMs});
		}
	}

	void NetMatchService::EndAdmissionSession() {
		if (m_AdmissionAttached && m_IsHost && m_Session) {
			// P22: sent from the same call that clears the registry, so a client's record is provably
			// dead exactly here - not on a transport disconnect, a timeout or a match Complete.
			m_Session->EndHostedSession("the host ended the session");
		}
		m_SeatAuth.EndSession();
		m_ReconnectHost.EndHostedSession();
		m_ReconnectHost.TakeOutbound();
		m_SeatStatuses.clear();
		m_SeatPresence.Clear();
		ResetRosterTransitionHistory();
		m_ModerationSeats.clear();
		m_AdmissionAttached = false;
	}

	void NetMatchService::RunCleanLeave() {
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_LeaveExchangeRun) {
				return;
			}
			m_LeaveExchangeRun = true;
			// Without a live link the recovery ticket must survive.
			if (!m_AdmissionAttached || m_IsHost || !m_Session || !m_Session->IsReady() || !m_TicketStore.HasRecord()) {
				return;
			}
			std::string error;
			if (!m_ReconnectClient.BeginLeave(AdmissionNowMs(), &error)) {
				return;
			}
		}
		for (;;) {
			{
				// The lock is dropped for the wait, so the game thread's own reads never stall on it.
				std::lock_guard<std::mutex> lock(m_Mutex);
				if (!m_Session) {
					break;
				}
				const uint64_t nowMs = AdmissionNowMs();
				m_Session->Tick(nowMs);
				// The client deadline must run even after the transport closes.
				m_ReconnectClient.Tick(nowMs);
				if (m_ReconnectClient.GetState() != NetH4ClientState::Leaving || m_ReconnectClient.WantsLinkClosed()) {
					break;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::cout << "[net-reconnect] leave: " << NetReconnectClientStateName(m_ReconnectClient.GetState())
		          << (m_TicketStore.HasRecord() ? " (ticket kept)" : " (ticket cleared)") << std::endl;
	}

	void NetMatchService::LeaveWorkerMain(std::string result) {
		RunCleanLeave();
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_Coordinator) {
			m_Coordinator->Leave(result);
		}
		m_WorkerDone = true;
	}

	void NetMatchService::WaitForPendingWork() {
		if (m_Worker.joinable()) {
			m_Worker.join();
		}
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_WorkerDone = false;
	}

	void NetMatchService::Destroy() {
		m_CancelRequested.store(true);
		if (m_Worker.joinable()) {
			m_Worker.join();
		}
		RunCleanLeave();
		m_LanDiscovery.Stop();
		ScenarioRunner::SetLockstepCoordinator(nullptr);
		ScenarioRunner::SetSessionPump(nullptr);
		std::unique_ptr<NetMatchRunner> runner;
		std::unique_ptr<NetLockstepCoordinator> coordinator;
		std::unique_ptr<NetSession> session;
		std::unique_ptr<GnsTransport> transport;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			runner = std::move(m_Runner);
			coordinator = std::move(m_Coordinator);
			session = std::move(m_Session);
			transport = std::move(m_Transport);
			m_WorkerDone = false;
			m_IsHost = false;
			m_LocalPeerId = 0;
			m_LocalTeam = -1;
			m_ResyncOnDesync = false;
			m_PendingResyncLoad.clear();
			m_PendingResyncState.reset();
			m_ResyncRetainsLocalState = false;
			m_ResyncSourceRound = 0;
			m_LocalName.clear();
			m_ActivityPreset.clear();
			m_State = NetMatchServiceState::Idle;
			m_StatusText = "Idle";
			m_ErrorText.clear();
			m_LobbySnapshot = {};
			m_InputDelayText.clear();
			m_LeaveExchangeRun = false;
			m_MatchWasRunning = false;
			EndAdmissionSession();
		}
		runner.reset();
		coordinator.reset();
		session.reset();
		transport.reset();
	}

	void NetMatchService::ReportRuntimeError(const std::string& error) {
		m_CancelRequested.store(true);
		if (m_Worker.joinable()) {
			m_Worker.join();
		}
		ScenarioRunner::SetLockstepCoordinator(nullptr);
		ScenarioRunner::SetSessionPump(nullptr);
		std::unique_ptr<NetMatchRunner> runner;
		std::unique_ptr<NetLockstepCoordinator> coordinator;
		std::unique_ptr<NetSession> session;
		std::unique_ptr<GnsTransport> transport;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_CapturedRunnerReport.empty() && m_Runner && m_Session && m_Coordinator) {
				m_CapturedRunnerReport = m_Runner->BuildReportJson(*m_Session, *m_Coordinator);
			}
			runner = std::move(m_Runner);
			coordinator = std::move(m_Coordinator);
			session = std::move(m_Session);
			transport = std::move(m_Transport);
			m_WorkerDone = false;
			m_IsHost = false;
			m_LocalPeerId = 0;
			m_LocalTeam = -1;
			m_ResyncOnDesync = false;
			m_PendingResyncLoad.clear();
			m_LocalName.clear();
			m_State = NetMatchServiceState::Failed;
			m_StatusText = "Match stopped";
			m_ErrorText = error;
			m_LobbySnapshot = {};
			m_InputDelayText.clear();
			EndAdmissionSession();
		}
		runner.reset();
		coordinator.reset();
		session.reset();
		transport.reset();
	}

	void NetMatchService::Complete(const std::string& reason) {
		// The recording gets its end marker at the match's end, not at process exit.
		ScenarioRunner::CloseLockstepReplayRecord();
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_Coordinator) {
			m_Coordinator->Complete(reason);
		}
		if (m_State == NetMatchServiceState::Running) {
			m_StatusText = reason.empty() ? "Match complete" : reason;
			m_ErrorText.clear();
		}
	}

	// Terminal clean end; the session objects stay alive for the next Start or quit.
	void NetMatchService::FinishMatch(const std::string& result) {
		ScenarioRunner::SetLockstepCoordinator(nullptr);
		ScenarioRunner::SetSessionPump(nullptr);
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_Coordinator) {
			m_Coordinator->Complete(result.empty() ? "match over" : result);
		}
		if (m_State == NetMatchServiceState::Running) {
			m_State = NetMatchServiceState::Completed;
			m_StatusText = result.empty() ? "Match complete" : result;
			m_ErrorText.clear();
		}
	}

	void NetMatchService::LeaveMatch(const std::string& result) {
		ScenarioRunner::SetLockstepCoordinator(nullptr);
		ScenarioRunner::SetSessionPump(nullptr);
		const std::string reason = result.empty() ? std::string("player left") : result;
		bool exchangeOwed = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			// §7 runs while the link is still up, so the round is told only once the ack has settled or
			// the P21 budget has run out. Without a ticket to answer for there is nothing to wait on.
			exchangeOwed = !m_LeaveExchangeRun && m_AdmissionAttached && !m_IsHost && m_Session &&
			               m_Session->IsReady() && m_TicketStore.HasRecord();
			if (!exchangeOwed && m_Coordinator) {
				m_Coordinator->Leave(reason);
			}
			if (m_State == NetMatchServiceState::Running) {
				m_State = NetMatchServiceState::Completed;
				m_StatusText = result.empty() ? "Left the match" : result;
				m_ErrorText.clear();
			}
		}
		if (!exchangeOwed) {
			return;
		}
		// The wait belongs to the worker: a sim thread must never sleep on a network answer.
		WaitForPendingWork();
		m_Worker = std::thread(&NetMatchService::LeaveWorkerMain, this, reason);
	}

	void NetMatchService::Update() {
		// Both the multiplayer screen and the menu loop's recovery pump call this; one pump per
		// millisecond is one pump per frame at any frame rate a menu runs at.
		const uint64_t nowMs = SteadyNowMs();
		if (m_LastUpdateMs == nowMs) {
			return;
		}
		m_LastUpdateMs = nowMs;
		JoinWorkerIfDone();
		// A hosting lobby advertises itself on the LAN until the match launches.
		bool beaconWanted = false;
		NetLobbySnapshot snapshot;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			beaconWanted = m_IsHost && m_State == NetMatchServiceState::Starting;
			if (beaconWanted) {
				snapshot = m_LobbySnapshot;
			}
		}
		if (beaconWanted) {
			std::string ignored;
			(void)m_LanDiscovery.StartBeacon(m_BeaconGamePort,
			                                 m_LocalName.empty() ? "Host" : m_LocalName,
			                                 snapshot.activityPreset.empty() ? m_ActivityPreset : snapshot.activityPreset,
			                                 snapshot.modeName,
			                                 static_cast<uint8_t>(std::max<size_t>(snapshot.members.size(), 1)),
			                                 m_BeaconMaxPlayers, &ignored);
			m_LanDiscovery.Tick(nowMs);
		} else if (m_LanDiscovery.IsBeaconing()) {
			m_LanDiscovery.Stop();
		}
		DriveReconnectUx(nowMs);
	}

	uint64_t NetMatchService::AdmissionNowMs() const {
		return m_AdmissionClock.NowMs(SteadyNowMs());
	}

	bool NetMatchService::NeedsRecoveryPump() const {
		if (!s_AdmissionEnabled) {
			return false;
		}
		const NetReconnectUxState uxState = m_ReconnectUx.GetState();
		if (uxState == NetReconnectUxState::Waiting || uxState == NetReconnectUxState::Retrying) {
			return true;
		}
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_State != NetMatchServiceState::Failed || m_IsHost || !m_MatchWasRunning) {
			return false;
		}
		return m_TicketStore.HasRecord();
	}

	void NetMatchService::DriveReconnectUx(uint64_t nowMs) {
		NetMatchServiceState state = NetMatchServiceState::Idle;
		bool isHost = false;
		bool hasRecord = false;
		bool matchWasRunning = false;
		std::string reason;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			state = m_State;
			isHost = m_IsHost;
			hasRecord = m_TicketStore.HasRecord();
			matchWasRunning = m_MatchWasRunning;
			reason = m_ErrorText;
		}
		if (state == NetMatchServiceState::Running || state == NetMatchServiceState::ReadyToLaunch) {
			if (m_ReconnectUx.IsActive()) {
				m_ReconnectUx.NoteReconnected(nowMs);
			} else if (m_ReconnectUx.GetState() == NetReconnectUxState::Idle) {
				m_ReconnectUx.NoteConnected(nowMs);
			}
			return;
		}
		// §11's same-process loss: the link died mid-MATCH and we still hold the record that proves the
		// seat. A lobby that never started is not a match to reclaim; losing one goes back to the menu.
		if (!s_AdmissionEnabled || state == NetMatchServiceState::Starting) {
			return;
		}
		if (!NetReconnectUx::RecoveryApplies(state == NetMatchServiceState::Failed, isHost, hasRecord, matchWasRunning)) {
			return;
		}
		if (m_ReconnectUx.GetState() == NetReconnectUxState::Retrying) {
			m_ReconnectUx.NoteAttemptFailed(nowMs, reason);
		} else {
			m_ReconnectUx.NoteDropped(nowMs, reason);
		}
		if (!m_ReconnectUx.Tick(nowMs)) {
			return;
		}
		m_ReconnectUx.NoteAttemptStarted(nowMs);
		std::string attemptError;
		if (!BeginTicketRejoin(&attemptError)) {
			m_ReconnectUx.NoteAttemptFailed(nowMs, attemptError);
		}
	}

	void NetMatchService::SetReady() {
		m_ReadyRequested.store(true);
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_State == NetMatchServiceState::Starting) {
			m_StatusText = "Ready; waiting for host start";
			m_ErrorText.clear();
		}
	}

	void NetMatchService::RequestStart() {
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_State == NetMatchServiceState::Idle) {
				m_StatusText = "Host a match before starting";
				m_ErrorText.clear();
				return;
			}
			if (!m_IsHost) {
				m_StatusText = "Only the host can start the match";
				m_ErrorText.clear();
				return;
			}
			if (m_State == NetMatchServiceState::Starting) {
				m_StatusText = "Start requested; waiting for peer";
				m_ErrorText.clear();
			}
		}
		m_StartRequested.store(true);
	}

	bool NetMatchService::ConsumeReadyToLaunch(std::string& outActivityPreset) {
		Update();
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_State != NetMatchServiceState::ReadyToLaunch || !m_Coordinator) {
			return false;
		}
		ScenarioRunner::SetLockstepCoordinator(m_Coordinator.get(), m_PendingResyncState.has_value());
		m_Coordinator->DeferStopsToTickBoundary();
		m_Coordinator->SetSeatStateSource(&NetMatchService::QuerySeatState, this);
		// The lockstep wait parks the sim thread; without this the plane could not answer a leave or a
		// reclaim while the round waits on the very peer that sent it.
		ScenarioRunner::SetSessionPump([this] { PumpSessionEvents(); });
		ScenarioRunner::SetLockstepSeatPresence(&m_SeatPresence);
		// The coordinator owns the transport queue during the match; reconnect handshakes hand over
		// here and drain through PumpSessionEvents on the same (game) thread.
		m_PendingSessionEvents.clear();
		m_Coordinator->SetSessionEventSink([this](const NetTransportEvent& event) {
			m_PendingSessionEvents.push_back(event);
		});
		if (m_Runner) {
			std::string recordError;
			(void)ScenarioRunner::BeginLockstepReplayRecord(m_Runner->GetMatchConfig(), &recordError);
		}
		outActivityPreset = m_ActivityPreset;
		m_MatchWasRunning = true;
		if (!m_PendingResyncState.has_value()) {
			ResetRosterTransitionHistory();
		}
		m_State = NetMatchServiceState::Running;
		m_StatusText = "Match running";
		CaptureA7SeatView();
		return true;
	}

	void NetMatchService::PumpSeatPresence() {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_Coordinator || m_State != NetMatchServiceState::Running) {
			return;
		}
		if (const auto snapshot = m_Coordinator->TakeSeatSnapshot()) {
			if (m_SeatPresence.ApplySnapshot(*snapshot)) {
				RecordRosterTransitions(snapshot->observedAtMs);
			}
		}
		m_SeatPresence.NoteFrame(ScenarioRunner::GetLockstepAppliedFrame());
		CaptureA7SeatView();
	}

	void NetMatchService::PublishModerationView() {
		if (!m_Coordinator || !m_IsHost || !m_AdmissionAttached || m_State != NetMatchServiceState::Running) return;
		auto seats = m_ReconnectHost.GetModerationView();
		const uint64_t appliedFrame = ScenarioRunner::GetLockstepAppliedFrame();
		const auto& leaves = m_Coordinator->GetPeerLeaveFrames();
		std::vector<NetSeatPresenceEntry> presence;
		for (auto& seat: seats) {
			if (seat.cpu || seat.lockstepPeerId == 0) continue;
			for (const auto& member: m_LobbySnapshot.members) {
				if (member.peerId == seat.lockstepPeerId) { seat.displayName = member.displayName; break; }
			}
			if (!seat.substituteName.empty()) seat.displayName = seat.substituteName;
			if (seat.displayName.empty()) {
				for (const auto& previous: m_ModerationSeats) {
					if (previous.stableSeat == seat.stableSeat && previous.epoch == seat.epoch) { seat.displayName = previous.displayName; break; }
				}
			}
			NetSeatPresenceEntry entry;
			entry.stableSeat = seat.stableSeat;
			entry.peerId = seat.lockstepPeerId;
			entry.holderName = seat.displayName;
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
				seat.holdFramesRemaining = appliedFrame < entry.holdUntilFrame ? entry.holdUntilFrame - appliedFrame : 0;
			}
			presence.push_back(std::move(entry));
		}
		std::sort(presence.begin(), presence.end(), [](const auto& a, const auto& b) { return a.stableSeat < b.stableSeat; });
		m_ModerationSeats = std::move(seats);
		(void)m_Coordinator->PublishSeatSnapshot(std::move(presence), AdmissionNowMs());
		if (const auto snapshot = m_Coordinator->TakeSeatSnapshot()) {
			if (m_SeatPresence.ApplySnapshot(*snapshot)) {
				RecordRosterTransitions(snapshot->observedAtMs);
			}
		}
	}

	void NetMatchService::PumpSessionEvents() {
		PumpSeatPresence();
		const bool hostAdmission = m_AdmissionAttached && m_IsHost;
		const bool holdPause = m_Coordinator && m_Coordinator->AnyDroppedSeatHeld();
		if (m_PendingSessionEvents.empty() && !hostAdmission && !holdPause) {
			return;
		}
		std::vector<NetTransportEvent> events;
		events.swap(m_PendingSessionEvents);
		bool answerMatchOver = false;
		std::string rejoinName;
		{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_Session || m_State != NetMatchServiceState::Running) {
			return;
		}
		// This is the sim thread inside the tick, so the drop-frame ownership census may walk the world
		// here and nowhere else. The scope is what makes that a checked property rather than a comment.
		const SimCensusScope censusScope;
		if (hostAdmission) {
			// Phase A: a ticketless join into a running match is refused; a returning holder proves.
			m_ReconnectHost.SetLiveMatch(true);
			m_Session->SetLockstepFrame(m_Coordinator ? m_Coordinator->GetStats().nextFrame : 0);
		}
		const uint64_t nowMs = AdmissionNowMs();
		// F1.5: the two clocks must be one. Sampled here, at the pump, because the report is written
		// after the loop stops feeding the session and its difference reads the teardown by then.
		if (const uint64_t sessionMs = m_Session->GetClockMs(); sessionMs > nowMs) {
			const uint64_t divergenceMs = sessionMs - nowMs;
			uint64_t seen = m_MaxClockDivergenceMs.load();
			while (divergenceMs > seen && !m_MaxClockDivergenceMs.compare_exchange_weak(seen, divergenceMs)) {
			}
		}
		if (hostAdmission) {
			// The coordinator owns the transport queue mid-match, so the session's own Tick never runs;
			// without this the plane's clock stops and a delayed refusal, an offer retransmit or a
			// dropped seat's reclaim window would wait for the match to end.
			m_Session->TickAdmissionPlane(nowMs);
		}
		if (holdPause && m_Session) {
			m_Session->TickKeepalive(nowMs);
		}
		for (const NetTransportEvent& event: events) {
			m_Session->InjectEvent(event, nowMs);
		}
		if (hostAdmission) {
			// The reseat is a lockstep command, not a local mutation: every peer applies the identical
			// ownership at the identical tick. QueueLocalInput restamps the sender as this peer, which
			// on the host is exactly the id MovableMan's reseat gate requires.
			DriveAutoSubstitution(nowMs);
			for (const NetGameReseat& reseat: m_ReconnectHost.TakePendingReseats()) {
				std::cout << "[net-reconnect] reseating team " << reseat.team << " onto peer "
				          << static_cast<int>(reseat.newOwnerPeerId) << " (" << reseat.actorUIDs.size() << " actors)" << std::endl;
				ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{ScenarioRunner::GetLockstepHostPeerId(), reseat});
			}
			if (m_Coordinator) {
				for (const NetHoldResolutionNotice& notice: m_ReconnectHost.TakePendingHoldResolutions()) {
					NetLockstepHoldResolution resolution = NetLockstepHoldResolution::None;
					switch (notice.resolution) {
						case NetHoldResolution::Expired: resolution = NetLockstepHoldResolution::Expired; break;
						case NetHoldResolution::Reclaimed: resolution = NetLockstepHoldResolution::Reclaimed; break;
						case NetHoldResolution::Substituted: resolution = NetLockstepHoldResolution::Substituted; break;
					}
					m_Coordinator->ResolveHeldSeat(notice.lockstepPeerId, resolution, nowMs);
				}
			}
			m_SeatStatuses = m_ReconnectHost.GetSeatStatuses();
			PublishModerationView();
			CaptureA7SeatView();
		}
		if (events.empty()) {
			return;
		}
		// A transport peer that reached session-Ready but carries no lockstep remote is a
		// reconnector: the host ends the round so everyone reconvenes around its snapshot.
		if (m_IsHost && m_ResyncOnDesync && m_Coordinator && m_Coordinator->IsRunning()) {
			for (const NetSessionPeerInfo& peer: m_Session->GetReadyPeers()) {
				if (!m_Coordinator->UsesTransportPeer(peer.transportPeerId)) {
					rejoinName = peer.displayName.empty() ? "a player" : peer.displayName;
					if (ClassifyRejoin(g_ActivityMan.GetActivity()) == NetRejoinAnswer::MatchOver) {
						answerMatchOver = true;
					} else {
						std::cout << "[net-match] rejoin: " << rejoinName << " reconnected - resyncing the match" << std::endl;
						m_Coordinator->RequestResync("player rejoined");
					}
					break;
				}
			}
		}
		}
		if (answerMatchOver) {
			std::cout << "[net-match] rejoin: " << rejoinName << " reconnected - match is over" << std::endl;
			AnswerMatchOverRejoin("match over");
		}
	}

	NetMatchServiceState NetMatchService::GetState() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_State;
	}

	NetLobbySnapshot NetMatchService::GetLobbySnapshot() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		NetLobbySnapshot snapshot = m_LobbySnapshot;
		snapshot.serviceState = StateName(m_State);
		snapshot.statusText = m_StatusText;
		if (!m_ErrorText.empty()) snapshot.errorText = m_ErrorText;
		snapshot.isHost = m_IsHost;
		snapshot.localPeerId = m_LocalPeerId;
		snapshot.localTeam = m_LocalTeam;
		snapshot.active = m_State != NetMatchServiceState::Idle;
		snapshot.inLobby = m_State == NetMatchServiceState::Starting;
		snapshot.running = m_State == NetMatchServiceState::Running || m_State == NetMatchServiceState::ReadyToLaunch;
		snapshot.failed = m_State == NetMatchServiceState::Failed;
		snapshot.inputDelayText = m_InputDelayText;
		if (snapshot.activityPreset.empty()) {
			snapshot.activityPreset = m_ActivityPreset;
		}
		if (snapshot.members.empty() && snapshot.active) {
			NetLobbyMember local;
			local.peerId = m_LocalPeerId;
			local.displayName = m_LocalName;
			local.team = m_LocalTeam >= 0 ? static_cast<uint8_t>(m_LocalTeam) : 0;
			local.isLocal = true;
			local.connected = true;
			snapshot.members.push_back(local);
		}
		for (NetLobbyMember& member: snapshot.members) {
			const auto current = m_SeatPresence.GetSeats().find(member.peerId);
			if (current != m_SeatPresence.GetSeats().end() && !current->second.holderName.empty()) member.displayName = current->second.holderName;
			const NetSeatPresenceState state = m_SeatPresence.StateOf(member.peerId);
			member.dropped = state == NetSeatPresenceState::Disconnected || state == NetSeatPresenceState::Reconnecting;
			member.reclaiming = state == NetSeatPresenceState::Reconnecting;
			member.statusLine = m_SeatPresence.Line(member.peerId, member.displayName);
		}
		return snapshot;
	}

	std::string NetMatchService::GetInputDelayText() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_InputDelayText;
	}

	std::string NetMatchService::GetStatusText() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_StatusText;
	}

	std::string NetMatchService::GetErrorText() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_ErrorText.empty() ? m_LobbySnapshot.errorText : m_ErrorText;
	}

	std::string NetMatchService::BuildReportJson() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		json report{
			{"state", StateName(m_State)},
			{"status", m_StatusText},
			{"error", m_ErrorText.empty() ? m_LobbySnapshot.errorText : m_ErrorText},
			{"activity_preset", m_ActivityPreset},
			{"is_host", m_IsHost},
			{"local_peer_id", static_cast<int>(m_LocalPeerId)},
			{"local_team", m_LocalTeam},
		};
		json reconnect{
			{"admission_enabled", s_AdmissionEnabled},
			{"admission_attached", m_AdmissionAttached},
			{"ux_state", NetReconnectUx::StateName(m_ReconnectUx.GetState())},
			{"ux_attempts", m_ReconnectUx.GetAttempts()},
			{"ticket_store_path", m_TicketStore.GetPath()},
			{"ticket_stored", m_TicketStore.HasRecord()},
			{"ticket_stores", m_TicketStore.GetStores()},
			{"ticket_clears", m_TicketStore.GetClears()},
			{"ticket_refused_loads", m_TicketStore.GetRefusedLoads()},
			// Every census must come from the sim tick; anything else means a drop was seen from a
			// thread that may not walk the world, and the ledger recorded nothing for it.
			{"census_refusals", m_CensusRefusals.load()},
			// Elapsed milliseconds, so a gate can tell a real deadline from a counted pump.
			{"admission_clock_ms", AdmissionNowMs()},
			// Zero whenever setup, play and every resync read one clock; the inflation itself otherwise.
			{"clock_divergence_max_ms", m_MaxClockDivergenceMs.load()},
			{"client_state", NetReconnectClientStateName(m_ReconnectClient.GetState())},
			{"client_used_stored_ticket", m_ReconnectClient.UsedStoredTicket()},
			{"client_reclaim_outcome", m_ReconnectClient.ReclaimOutcome()},
			{"client_commits", m_ReconnectClient.GetStats().commitsReceived},
			{"client_leave_acks", m_ReconnectClient.GetStats().leaveAcksReceived},
			{"client_ambiguous_losses", m_ReconnectClient.GetStats().ambiguousLosses},
			{"client_unacknowledged_leaves", m_ReconnectClient.GetStats().unacknowledgedLeaves},
			{"client_retransmits", m_ReconnectClient.GetStats().retransmits},
			{"client_confirmed_session_ends", m_ReconnectClient.GetStats().confirmedSessionEnds},
			{"client_applications_sent", m_ReconnectClient.GetStats().applicationsSent},
			{"client_applications_acknowledged", m_ReconnectClient.GetStats().applicationsAcknowledged},
			{"client_substitution_offers", m_ReconnectClient.GetStats().substitutionOffersReceived},
			{"client_substitution_acks", m_ReconnectClient.GetStats().substitutionAcksSent},
			{"client_reject_reason", m_ReconnectClient.HasLastRejectReason() ? NetProtocol::RejectReasonName(m_ReconnectClient.GetLastRejectReason()) : ""},
			// The host's drop-and-reseat accounting, which survives the session end the seat list does
			// not: after the activity ends the seats read empty, so these are all a gate has left.
			{"host_seats_dropped", m_ReconnectHost.GetStats().seatsDropped},
			{"host_ledger_drops_recorded", m_ReconnectHost.GetStats().ledgerDropsRecorded},
			{"host_reseats_issued", m_ReconnectHost.GetStats().reseatsIssued},
			{"host_reseats_without_a_ledger", m_ReconnectHost.GetStats().reseatsWithoutALedger},
			{"host_reseats_without_survivors", m_ReconnectHost.GetStats().reseatsWithoutSurvivors},
			{"host_reseat_live_on_team_not_named", m_ReconnectHost.GetStats().reseatLiveOnTeamNotNamed},
		};
		json seats = json::array();
		for (const NetH4SeatStatus& seat: m_SeatStatuses) {
			seats.push_back({{"stable_seat", seat.stableSeat},
			                 {"lockstep_peer_id", static_cast<int>(seat.lockstepPeerId)},
			                 {"committed", seat.committed},
			                 {"closed", seat.closed},
			                 {"dropped", seat.dropped},
			                 {"reclaiming", seat.reclaiming},
			                 {"substituting", seat.substituting},
			                 {"applicants", seat.applicants}});
		}
		reconnect["seats"] = seats;
		// §9b's moderation view, so a gate can read what the host was offered and what it decided.
		json moderation = json::array();
		if (m_AdmissionAttached && m_IsHost) {
			for (const NetH4ModerationSeat& seat: m_ModerationSeats) {
				json applicants = json::array();
				for (const NetH4ApplicantView& applicant: seat.applicants) {
					applicants.push_back(json{
						{"connection", applicant.connection},
						{"display_name", applicant.displayName},
						{"approved", applicant.approved},
						{"transaction_id", applicant.transactionId},
					});
				}
				moderation.push_back(json{
					{"stable_seat", seat.stableSeat},
					{"lockstep_peer_id", static_cast<int>(seat.lockstepPeerId)},
					{"cpu", seat.cpu},
					{"display_name", seat.displayName},
					{"epoch", seat.epoch},
					{"incarnation", seat.incarnation},
					{"hold_frames_remaining", seat.holdFramesRemaining},
					{"substitution_transaction", seat.substitutionTransaction},
					{"actions_available", m_State == NetMatchServiceState::Running},
					{"committed", seat.committed},
					{"dropped", seat.dropped},
					{"closed", seat.closed},
					{"held_for_reclaim", seat.heldForReclaim},
					{"substitutable", seat.substitutable},
					{"substituting", seat.substituting},
					{"holder_generation", seat.holderGeneration},
					{"seat_generation", seat.seatGeneration},
					{"dropped_for_ms", seat.droppedForMs},
					{"applicants", applicants},
				});
			}
		}
		reconnect["moderation"] = moderation;
		// §11's roster lines exactly as this peer shows them, so a two-process gate can read a CLIENT's.
		json rosterLines = json::array();
		for (const NetLobbyMember& member: m_LobbySnapshot.members) {
			const std::string line = m_SeatPresence.Line(member.peerId, member.displayName);
			if (!line.empty()) {
				rosterLines.push_back(json{
					{"peer_id", static_cast<int>(member.peerId)},
					{"state", NetSeatPresence::StateName(m_SeatPresence.StateOf(member.peerId))},
					{"line", line},
				});
			}
		}
		reconnect["roster_lines"] = rosterLines;
		json rosterTransitions = json::array();
		for (const RosterTransition& row: m_RosterTransitions) {
			rosterTransitions.push_back(json{
				{"peer_id", static_cast<int>(row.peerId)},
				{"state", row.state},
				{"line", row.line},
				{"applied_frame", row.appliedFrame},
				{"observed_at_ms", row.observedAtMs},
			});
		}
		reconnect["roster_transitions"] = rosterTransitions;
		reconnect["roster_transitions_dropped"] = static_cast<int>(m_RosterTransitionsDropped);
		if (const auto& snapshot = m_SeatPresence.GetSnapshot()) {
			json entries = json::array();
			for (const auto& seat: snapshot->seats) {
				entries.push_back({{"stable_seat", seat.stableSeat}, {"peer_id", seat.peerId},
				                   {"state", NetSeatPresence::StateName(seat.state)}, {"holder_name", seat.holderName},
				                   {"holder_generation", seat.holderGeneration}, {"seat_generation", seat.seatGeneration},
				                   {"incarnation", seat.incarnation}, {"hold_until_frame", seat.holdUntilFrame},
				                   {"hold_active", seat.holdActive}, {"hold_until_ms", seat.holdUntilMs}});
			}
			reconnect["seat_snapshot"] = {{"epoch", snapshot->epoch}, {"session_id", snapshot->sessionId},
			                              {"round_id", snapshot->roundId}, {"revision", snapshot->revision},
			                              {"observed_at_ms", snapshot->observedAtMs}, {"seats", entries}};
		}
		if (!m_RejoinOutcome.empty()) {
			reconnect["rejoin_outcome"] = m_RejoinOutcome;
		}
		report["reconnect"] = reconnect;
		if (m_Runner && m_Session && m_Coordinator) {
			report["runner"] = json::parse(m_Runner->BuildReportJson(*m_Session, *m_Coordinator));
		} else if (!m_CapturedRunnerReport.empty()) {
			report["runner"] = json::parse(m_CapturedRunnerReport);
		} else {
			const NetReconnectHostStats& stats = m_ReconnectHost.GetStats();
			report["runner"] = json{
				{"session",
				 {{"admission", AdmissionJsonFromHost(m_ReconnectHost)},
				  {"stats", {{"fenced_disconnects", stats.fencedDisconnects}, {"fenced_packets", stats.fencedPackets}}}}},
			};
		}
		return report.dump();
	}

	uint8_t NetMatchService::GetLocalPeerId() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_LocalPeerId;
	}

	int NetMatchService::GetLocalTeam() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_LocalTeam;
	}

	const char* NetMatchService::StateName(NetMatchServiceState state) {
		switch (state) {
			case NetMatchServiceState::Idle: return "Idle";
			case NetMatchServiceState::Starting: return "Starting";
			case NetMatchServiceState::ReadyToLaunch: return "ReadyToLaunch";
			case NetMatchServiceState::Running: return "Running";
			case NetMatchServiceState::Completed: return "Completed";
			case NetMatchServiceState::Failed: return "Failed";
		}
		return "Unknown";
	}

	void NetMatchService::WorkerMain(NetMatchServiceRequest request, NetIdentityManifest manifest) {
		if (request.host) {
			// Arm the off-sim reconnect-auth epoch; without real crypto nothing is issued (fail closed).
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_SeatAuth.BeginHostedSession()) {
				std::cout << "[net-auth] reconnect-auth epoch armed" << std::endl;
			} else {
				std::cout << "[net-auth] crypto unavailable - reconnect auth disabled" << std::endl;
			}
		}
		auto transport = std::make_unique<GnsTransport>();
		auto session = std::make_unique<NetSession>();
		auto coordinator = std::make_unique<NetLockstepCoordinator>();
		auto runner = std::make_unique<NetMatchRunner>();

		NetMatchRunnerConfig runnerConfig;
		runnerConfig.host = request.host;
		runnerConfig.joinAddress = request.host ? "" : request.address;
		runnerConfig.sessionConfig = BuildSessionConfig(manifest, request);
		runnerConfig.matchConfig = BuildMatchConfig(request, runnerConfig.sessionConfig.sessionId);
		runnerConfig.autoInputDelay = request.autoInputDelay;
		runnerConfig.useLobbyProtocol = true;
		// Wait patiently for the other player to connect (host listening / client retrying), not the 15s default.
		runnerConfig.sessionWaitMs = c_MenuLobbyWaitMs;
		runnerConfig.lobbyWaitMs = c_MenuLobbyWaitMs;
		// First lockstep tick is 1: RestartActivity zeroes the sim count, UpdateSim increments it before MovableMan reads it.
		runnerConfig.startFrame = 1;
		runnerConfig.scenario = request.activityPreset;
		runnerConfig.autoReady = request.host;
		runnerConfig.autoStart = false;
		runnerConfig.readyRequested = &m_ReadyRequested;
		runnerConfig.startRequested = &m_StartRequested;
		runnerConfig.cancelRequested = &m_CancelRequested;
		NetMatchRunner* runnerRaw = runner.get();
		runnerConfig.publishLobby = [this, runnerRaw](const NetLobbySnapshot& snapshot) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_LobbySnapshot = snapshot;
			// The announced delay comes from the lobby's exchanged config (host-authored, already
			// auto-adjusted) — never recomputed here, so every peer renders the same value.
			const NetMatchConfig& config = runnerRaw->GetLobbySession().GetState() != NetLobbyState::Idle
			                                   ? runnerRaw->GetLobbySession().GetMatchConfig()
			                                   : runnerRaw->GetMatchConfig();
			uint8_t localPeerId = m_LocalPeerId;
			uint32_t pingMs = 0;
			for (const NetLobbyMember& member: snapshot.members) {
				if (member.isLocal) {
					localPeerId = member.peerId;
					pingMs = member.pingMs;
				}
			}
			m_InputDelayText = "Input delay: " + std::to_string(NetMatchConfigUtil::PeerInputDelay(config, localPeerId)) +
			    (config.peerInputDelayFrames.empty() ? " (fixed)" : " (auto, " + std::to_string(pingMs) + "ms ping)");
		};

		// One clock from here on: setup, play, stalls and every resync read the same elapsed time.
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_AdmissionClock.Start(SteadyNowMs());
		}
		runnerConfig.nowMs = [this] { return AdmissionNowMs(); };

		AttachAdmissionPlane(*session, request, runnerConfig.matchConfig, runnerConfig.sessionConfig, manifest);

		std::string error;
		bool started = runner->Start(*transport, *session, *coordinator, runnerConfig, &error);
		// A joiner whose lobby round carried a match state is RECONNECTING into a live match; it
		// launches from the received snapshot instead of a fresh activity. Launching a FRESH match
		// while the others play the snapshot would desync instantly, so a failed write fails the join.
		std::string pendingLoad;
		std::optional<NetResyncState> pendingState;
		if (started) {
			std::vector<uint8_t> receivedState = runner->TakeReceivedState();
			if (!receivedState.empty()) {
				NetResyncState state;
				started = PrepareReceivedResync(receivedState, *coordinator, pendingLoad, state, &error);
				if (started) pendingState = std::move(state);
			}
		}
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (started) {
				// The lockstep peer id is the session-assigned id + 1; the team comes from that slot.
				const uint8_t localLockstepId = static_cast<uint8_t>(session->GetLocalPeerId() + 1);
				int localTeam = m_LocalTeam;
				for (const NetMatchPlayerSlot& slot : runner->GetMatchConfig().players) {
					if (slot.peerId == localLockstepId) {
						localTeam = slot.team;
						break;
					}
				}
				m_Transport = std::move(transport);
				m_Session = std::move(session);
				m_Coordinator = std::move(coordinator);
				m_Runner = std::move(runner);
				m_LocalPeerId = localLockstepId;
				m_LocalTeam = localTeam;
				m_PendingResyncLoad = pendingLoad;
				m_PendingResyncState = std::move(pendingState);
				m_State = NetMatchServiceState::ReadyToLaunch;
				m_StatusText = "Ready to launch match";
				m_ErrorText.clear();
			} else {
				// Keep the objects on failure too — the report needs the session's reject record.
				m_Transport = std::move(transport);
				m_Session = std::move(session);
				m_Coordinator = std::move(coordinator);
				m_Runner = std::move(runner);
				m_State = NetMatchServiceState::Failed;
				m_StatusText = "Network setup failed";
				m_ErrorText = error;
			}
			m_WorkerDone = true;
		}
	}

	bool NetMatchService::WaitForA7ConnectGate(std::string* error) {
		if (!NetA7Journal::HasConnectGate()) return true;
		NetReconnectTicketStore store;
		store.SetPath(s_TicketStorePath.empty() ? NetReconnectTicketStore::DefaultPath() : s_TicketStorePath);
		NetH4TicketRecord record;
		const auto result = store.Load(UnixNowMs(nullptr), record, nullptr);
		NetA7Journal::Emit("ticket_preload", {{"load_result", static_cast<int>(result)}, {"ticket_sha256", store.GetA7LoadedSha256()}});
		return NetA7Journal::WaitForConnectGate(store.GetA7LoadedSha256(), error);
	}

	bool NetMatchService::CanSealA7Journal() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return !m_Worker.joinable() || m_WorkerDone;
	}

	void NetMatchService::CaptureA7SeatView() {
		if (!NetA7Journal::Enabled()) return;
		json rows = json::array();
		if (m_IsHost && m_AdmissionAttached) {
			for (const auto& seat: m_ReconnectHost.GetSeatStatuses()) {
				rows.push_back({{"stable_seat", seat.stableSeat}, {"committed", seat.committed}, {"dropped", seat.dropped}, {"closed", seat.closed}});
			}
		}
		NetA7Journal::SetSeatView(std::move(rows), AdmissionNowMs(), m_IsHost ? "host_admission_plane" : "host_rows_unavailable_on_client");
	}

	void NetMatchService::SetTicketStorePath(std::string path) {
		s_TicketStorePath = std::move(path);
	}

	void NetMatchService::SetJoinWaitPath(std::string path) {
		s_JoinWaitPath = std::move(path);
	}

	bool NetMatchService::WaitForJoinTrigger(const std::string& path, uint64_t budgetMs, std::string* error) {
		if (path.empty()) return true;
		const std::filesystem::path trigger(path);
		const auto opened = std::chrono::steady_clock::now();
		std::cout << "[net-join-wait] waiting for " << path << std::endl;
		for (;;) {
			std::error_code fsError;
			if (std::filesystem::exists(trigger, fsError) && !fsError) {
				const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - opened).count();
				std::cout << "[net-join-wait] released after " << waitedMs << "ms" << std::endl;
				return true;
			}
			const uint64_t elapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - opened).count());
			if (elapsedMs >= budgetMs) break;
			const uint64_t remainingMs = budgetMs - elapsedMs;
			std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint64_t>(c_JoinWaitPollMs, remainingMs)));
		}
		if (error) *error = "join wait timed out: " + path + " did not appear within " + std::to_string(budgetMs) + "ms";
		std::cerr << "[net-join-wait] timed out after " << budgetMs << "ms waiting for " << path << std::endl;
		return false;
	}

	void NetMatchService::SetApplyForSeat(bool enabled, uint16_t stableSeat) {
		s_ApplyForSeat = enabled;
		s_ApplySeat = stableSeat;
	}

	void NetMatchService::SetAutoSubstitute(bool enabled, uint16_t stableSeat, uint64_t delayMs, bool thenCancel) {
		s_AutoSubstitute = enabled;
		s_AutoSubstituteSeat = stableSeat;
		s_AutoSubstituteDelayMs = delayMs;
		s_AutoSubstituteThenCancel = thenCancel;
	}

	std::vector<NetH4ModerationSeat> NetMatchService::GetModerationSeats() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_AdmissionAttached || !m_IsHost) {
			return {};
		}
		auto seats = m_ModerationSeats;
		for (auto& seat: seats) seat.actionsAvailable = m_State == NetMatchServiceState::Running;
		return seats;
	}

	NetH4ModerationResult NetMatchService::ApplyModeration(const NetModerationSelection& selection, NetModerationAction action) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_AdmissionAttached || !m_IsHost) {
			return NetH4ModerationResult::NotHosting;
		}
		if (m_State != NetMatchServiceState::Running || !m_Session || !m_Coordinator) {
			return NetH4ModerationResult::ActionUnavailable;
		}
		const uint64_t nowMs = AdmissionNowMs();
		const NetH4ModerationResult result = m_ReconnectHost.ApplyModeration(selection, action, nowMs);
		m_Session->TickAdmissionPlane(nowMs);
		PublishModerationView();
		return result;
	}

	void NetMatchService::DriveAutoSubstitution(uint64_t nowMs) {
		if (!s_AutoSubstitute || m_AutoSubstituteDone) {
			return;
		}
		for (const NetH4ModerationSeat& seat : m_ReconnectHost.GetModerationView()) {
			if (seat.stableSeat != s_AutoSubstituteSeat || !seat.substitutable || seat.applicants.empty()) {
				continue;
			}
			if (m_AutoSubstituteReadyMs == 0) {
				m_AutoSubstituteReadyMs = nowMs;
			}
			if (nowMs < m_AutoSubstituteReadyMs + s_AutoSubstituteDelayMs) {
				return;
			}
			const NetH4ModerationResult result = m_ReconnectHost.SubstituteApplicant(seat.stableSeat, seat.applicants.front().connection, nowMs);
			std::cout << "[net-reconnect] moderation: substitute seat " << seat.stableSeat << " -> "
			          << NetH4ModerationResultName(result) << std::endl;
			if (result == NetH4ModerationResult::Ok && s_AutoSubstituteThenCancel) {
				std::cout << "[net-reconnect] moderation: cancel seat " << seat.stableSeat << " -> "
				          << NetH4ModerationResultName(m_ReconnectHost.CancelSubstitution(seat.stableSeat, nowMs)) << std::endl;
			}
			m_AutoSubstituteDone = true;
			return;
		}
	}


	std::vector<NetH4LedgerActor> NetMatchService::CollectDropOwnership(void* context) {
		auto* service = static_cast<NetMatchService*>(context);
		if (!t_SimCensusOpen) {
			// A drop seen from the setup worker, not the sim tick. Walking g_MovableMan from there is a
			// data race, so the seat records an empty ledger rather than a torn one.
			if (service) {
				service->m_CensusRefusals.fetch_add(1);
			}
			return {};
		}
		std::vector<NetH4LedgerActor> actors;
		for (const MovableMan::LockstepActorOwner& owner: g_MovableMan.BuildLockstepOwnershipCensus()) {
			actors.push_back({owner.actorUID, owner.team, owner.ownerPeerId, true});
		}
		return actors;
	}

	NetLockstepSeatState NetMatchService::QuerySeatState(void* context, uint8_t lockstepPeerId, NetPeerId transportPeerId) {
		auto* service = static_cast<NetMatchService*>(context);
		NetLockstepSeatState state;
		if (!service) {
			return state;
		}
		std::lock_guard<std::mutex> lock(service->m_Mutex);
		if (!service->m_AdmissionAttached || !service->m_IsHost) {
			return state;
		}
		state.fencedTransport = transportPeerId != c_InvalidNetPeerId && service->m_ReconnectHost.IsFenced(transportPeerId);
		state.heldForReclaim = service->m_ReconnectHost.IsSeatHeldForReclaim(lockstepPeerId);
		return state;
	}

	void NetMatchService::AttachAdmissionPlane(NetSession& session, const NetMatchServiceRequest& request, const NetMatchConfig& matchConfig, const NetSessionConfig& sessionConfig, const NetIdentityManifest& manifest) {
		m_AdmissionAttached = false;
		if (!s_AdmissionEnabled) {
			return;
		}
		const NetH4Identity identity = BuildH4Identity(manifest);
		if (request.host) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (!m_SeatAuth.IsActive()) {
				// Fail closed: with no crypto nothing can be issued or proven, so the session keeps the
				// pre-admission handshake rather than gating every join on a ticket it cannot mint.
				return;
			}
			m_ReconnectHost.Configure(&m_SeatAuth, sessionConfig.sessionId, identity);
			m_ReconnectHost.SetSeatTable(NetH4BuildSeatTable(matchConfig), matchConfig.mode);
			m_ReconnectHost.SetHostAddress(NetLanDiscovery::GetPrimaryLocalAddress());
			m_ReconnectHost.SetMatchConfigHash(NetMatchConfigUtil::HashConfig(matchConfig));
			m_ReconnectHost.SetLiveMatch(false);
			m_ReconnectHost.SetDropOwnershipSource(&NetMatchService::CollectDropOwnership, this);
			session.SetReconnectHost(&m_ReconnectHost);
			m_AdmissionAttached = true;
			return;
		}
		m_TicketStore.SetPath(s_TicketStorePath.empty() ? NetReconnectTicketStore::DefaultPath() : s_TicketStorePath);
		m_ReconnectClient.Configure(&m_TicketStore, identity, request.playerName.empty() ? "Client" : request.playerName);
		m_ReconnectClient.SetUnixClock(&UnixNowMs, nullptr);
		// The record names the host it belongs to; the config hash is context, not a gate - a client
		// adopts the host's match config in the lobby round that follows.
		m_ReconnectClient.SetHostContext(request.address, NetHash32{});
		m_ReconnectClient.SetApplyForSeat(s_ApplyForSeat, s_ApplySeat);
		session.SetReconnectClient(&m_ReconnectClient);
		m_AdmissionAttached = true;
	}

	void NetMatchService::ScanStoredTicket() {
		if (!s_AdmissionEnabled) {
			m_ReconnectUx.DismissOffer();
			return;
		}
		m_TicketStore.SetPath(s_TicketStorePath.empty() ? NetReconnectTicketStore::DefaultPath() : s_TicketStorePath);
		NetH4TicketRecord record;
		const NetH4TicketLoadResult load = m_TicketStore.Load(UnixNowMs(nullptr), record, nullptr);
		m_ReconnectUx.OfferStoredTicket(load, record.hostAddress);
	}

	bool NetMatchService::BeginTicketRejoin(std::string* error) {
		NetH4TicketRecord record;
		m_TicketStore.SetPath(s_TicketStorePath.empty() ? NetReconnectTicketStore::DefaultPath() : s_TicketStorePath);
		const NetH4TicketLoadResult load = m_TicketStore.Load(UnixNowMs(nullptr), record, nullptr);
		m_ReconnectUx.OfferStoredTicket(load, record.hostAddress);
		if (load != NetH4TicketLoadResult::Loaded) {
			if (error) *error = m_ReconnectUx.GetOfferText().empty() ? "no reconnect ticket to rejoin with" : m_ReconnectUx.GetOfferText();
			return false;
		}
		NetMatchServiceRequest request;
		request.host = false;
		request.address = record.hostAddress;
		request.playerName = m_LocalName.empty() ? "Client" : m_LocalName;
		request.resyncOnDesync = true;
		return Start(request, error);
	}

	NetSessionConfig NetMatchService::BuildSessionConfig(const NetIdentityManifest& manifest, const NetMatchServiceRequest& request) const {
		NetSessionConfig config;
		config.localIdentity = manifest;
		config.displayName = request.playerName.empty() ? (request.host ? "Host" : "Client") : request.playerName;
		config.port = request.port;
		config.sessionId = c_UiSessionId;
		config.localNonce = request.host ? c_HostNonce : MakeClientNonce();
		config.maxPeers = static_cast<uint8_t>(std::max(1, static_cast<int>(request.peerCount) - 1));
		config.heartbeatIntervalMs = 50;
		config.timeoutMs = 5000;
		config.rejectUserdataModules = false;
		return config;
	}

	NetMatchConfig NetMatchService::BuildMatchConfig(const NetMatchServiceRequest& request, uint64_t sessionId) const {
		const uint8_t peerCount = std::clamp<uint8_t>(request.peerCount, NetMatchConfigUtil::c_MinPeerCount, NetMatchConfigUtil::c_MaxPeerCount);
		// PvPvE gives the CPU the team after the humans', so it fits three human teams at most.
		const NetMatchMode mode = (request.mode == NetMatchMode::PvPvE && peerCount >= 4) ? NetMatchMode::PvPSkirmish : request.mode;
		NetMatchConfig config = NetMatchConfigUtil::MakeDefault(sessionId);
		config.activityPreset = request.activityPreset.empty() ? "P4 Alpha Duel" : request.activityPreset;
		config.sceneName = "Grasslands";
		config.mode = mode;
		config.modePreset = NetMatchConfigUtil::ModeName(mode);
		config.ownershipPolicy = request.ownershipPolicy;
		config.inputDelayFrames = request.inputDelayFrames;
		config.peerCount = peerCount;
		// The host authors the roster; clients adopt it via the lobby config sync. PvP seats one team
		// per peer; co-op PvE seats every human on team 0; PvPvE keeps per-peer teams. The PvE modes
		// add a peerless CPU slot whose team the host's AI drives over the wire.
		config.players.clear();
		for (uint8_t peerId = 1; peerId <= peerCount; ++peerId) {
			NetMatchPlayerSlot slot;
			slot.peerId = peerId;
			slot.team = mode == NetMatchMode::CoopPvE ? 0 : static_cast<uint8_t>(peerId - 1);
			slot.cpu = false;
			slot.displayName = peerId == config.hostPeerId ? PlayerNameOrDefault(request, true)
			                                               : ("Client " + std::to_string(peerId));
			config.players.push_back(slot);
		}
		if (mode == NetMatchMode::CoopPvE || mode == NetMatchMode::PvPvE) {
			NetMatchPlayerSlot cpuSlot;
			cpuSlot.peerId = 0;
			cpuSlot.team = mode == NetMatchMode::CoopPvE ? 1 : peerCount;
			cpuSlot.cpu = true;
			cpuSlot.displayName = "CPU";
			config.players.push_back(cpuSlot);
		}
		return config;
	}

	void NetMatchService::SetState(NetMatchServiceState state, std::string status, std::string error) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_State = state;
		m_StatusText = std::move(status);
		m_ErrorText = std::move(error);
	}

	void NetMatchService::JoinWorkerIfDone() {
		bool done = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			done = m_WorkerDone;
		}
		if (done && m_Worker.joinable()) {
			m_Worker.join();
		}
	}

} // namespace RTE
