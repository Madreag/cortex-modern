#include "NetMatchService.h"

#include "ActivityMan.h"
#include "Constants.h"
#include "GameActivity.h"
#include "GnsTransport.h"
#include "NetIdentity.h"
#include "PresetMan.h"
#include "ScenarioRunner.h"
#include "System.h"
#include "TimerMan.h"

#include "MovableMan.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <utility>

namespace RTE {

	bool NetMatchService::s_AdmissionEnabled = true;
	std::string NetMatchService::s_TicketStorePath;

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

		m_ActivityPreset = request.activityPreset;
		SetState(NetMatchServiceState::Starting, request.host ? "Hosting direct-IP match" : "Joining direct-IP match");
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_WorkerDone = false;
			m_IsHost = request.host;
			m_LocalPeerId = request.host ? 1 : 2;
			m_LocalTeam = request.host ? 0 : 1;
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
		if (isHost) {
			if (!g_ActivityMan.SaveCurrentGame(ResyncSaveName()) || !g_ActivityMan.WaitForSaveGameTask()) {
				if (error) *error = "resync snapshot save failed";
				return false;
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
		}
		ScenarioRunner::SetLockstepCoordinator(nullptr);
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
				// The snapshot restores verbatim at its saved sim tick, so the healed round's first frame follows it.
				runner->SetStartFrame(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) + 1U);
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
		const bool started = runner->StartNextMatch(*transport, *session, *coordinator, &error, std::move(stateBytes));
		std::string pendingLoad;
		if (started) {
			std::vector<uint8_t> receivedState = runner->TakeReceivedState();
			if (!receivedState.empty()) {
				// Per-peer filename: same-machine instances share Userdata (the e2e), so concurrent
				// receivers must never write or load the same file.
				const std::string recvName = ResyncSaveName() + "_recv";
				const std::string recvPath = g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/" + recvName + ".ccsave";
				std::ofstream out(recvPath, std::ios::binary | std::ios::trunc);
				out.write(reinterpret_cast<const char*>(receivedState.data()), static_cast<std::streamsize>(receivedState.size()));
				out.close();
				pendingLoad = out.good() ? recvName : "";
				if (pendingLoad.empty()) {
					error = "could not write the received resync snapshot";
				}
			} else if (m_IsHost) {
				// The host reloads its own snapshot, so both sides launch the identical file.
				pendingLoad = ResyncSaveName();
			} else {
				// A non-host with no received bytes means the transfer never completed; loading the
				// host's filename off this peer's disk would restore a stale or absent snapshot.
				pendingLoad = "";
				error = "resync snapshot transfer did not complete";
			}
		}
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Transport = std::move(transport);
			m_Session = std::move(session);
			m_Coordinator = std::move(coordinator);
			m_Runner = std::move(runner);
			if (started && !pendingLoad.empty()) {
				m_PendingResyncLoad = pendingLoad;
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
		if (pendingLoad.empty()) {
			if (error) *error = "no resync snapshot to load";
			return false;
		}
		if (!g_ActivityMan.LoadGameToRestart(pendingLoad)) {
			if (error) *error = "resync snapshot load failed: " + pendingLoad;
			return false;
		}
		g_ActivityMan.RemoveSavedGame(pendingLoad);
		std::cout << "[net-match] launching from the received snapshot: " << pendingLoad << std::endl;
		const int localTeam = GetLocalTeam();
		if (localTeam < Activity::TeamOne || localTeam >= Activity::MaxTeamCount) {
			if (error) *error = "invalid local team";
			return false;
		}
		if (GameActivity* gameActivity = dynamic_cast<GameActivity*>(g_ActivityMan.GetStartActivity())) {
			gameActivity->ClearPlayers(false);
			gameActivity->AddPlayer(Players::PlayerOne, true, localTeam, 0);
		}
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
		m_AdmissionAttached = false;
	}

	void NetMatchService::RunCleanLeave() {
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_LeaveExchangeRun) {
				return;
			}
			m_LeaveExchangeRun = true;
			// A leave needs a live link. Without one this is an ambiguous loss, and §7 is explicit that
			// the ticket must survive that - which is exactly when it is needed.
			if (!m_AdmissionAttached || m_IsHost || !m_Session || !m_Session->IsReady() || !m_TicketStore.HasRecord()) {
				return;
			}
			// §7: leaving is a protocol, not a flag. Only an acknowledged LeaveAck clears the ticket, and
			// an unacknowledged one is an ambiguous loss that KEEPS it - so this waits exactly the P21
			// budget and no longer, whatever the answer.
			std::string error;
			if (!m_ReconnectClient.BeginLeave(m_Session->GetClockMs(), &error)) {
				return;
			}
		}
		const uint64_t startMs = SteadyNowMs();
		while (SteadyNowMs() - startMs <= NetReconnectClient::c_LeaveAckBudgetMs) {
			{
				// The lock is dropped for the wait, so the game thread's own reads never stall on it.
				std::lock_guard<std::mutex> lock(m_Mutex);
				if (!m_Session) {
					break;
				}
				m_Session->Tick(m_Session->GetClockMs() + 5);
				if (m_ReconnectClient.GetState() == NetH4ClientState::Left || m_ReconnectClient.WantsLinkClosed()) {
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
			m_LocalName.clear();
			m_ActivityPreset.clear();
			m_State = NetMatchServiceState::Idle;
			m_StatusText = "Idle";
			m_ErrorText.clear();
			m_LobbySnapshot = {};
			m_LeaveExchangeRun = false;
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
			m_LocalName.clear();
			m_State = NetMatchServiceState::Failed;
			m_StatusText = "Match stopped";
			m_ErrorText = error;
			m_LobbySnapshot = {};
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
		const uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
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

	void NetMatchService::DriveReconnectUx(uint64_t nowMs) {
		NetMatchServiceState state = NetMatchServiceState::Idle;
		bool isHost = false;
		bool hasRecord = false;
		std::string reason;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			state = m_State;
			isHost = m_IsHost;
			hasRecord = m_TicketStore.HasRecord();
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
		// §11's same-process loss: the link died and we still hold the record that proves the seat.
		if (!s_AdmissionEnabled || isHost || !hasRecord || state == NetMatchServiceState::Starting) {
			return;
		}
		if (state != NetMatchServiceState::Failed) {
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
		ScenarioRunner::SetLockstepCoordinator(m_Coordinator.get());
		m_Coordinator->DeferStopsToTickBoundary();
		m_Coordinator->SetSeatStateSource(&NetMatchService::QuerySeatState, this);
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
		m_State = NetMatchServiceState::Running;
		m_StatusText = "Match running";
		return true;
	}

	void NetMatchService::PumpSessionEvents() {
		const bool hostAdmission = m_AdmissionAttached && m_IsHost;
		if (m_PendingSessionEvents.empty() && !hostAdmission) {
			return;
		}
		std::vector<NetTransportEvent> events;
		events.swap(m_PendingSessionEvents);
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
		m_SessionPumpNowMs += 15;
		if (hostAdmission) {
			// The coordinator owns the transport queue mid-match, so the session's own Tick never runs;
			// without this the plane's clock stops and a delayed refusal, an offer retransmit or a
			// dropped seat's reclaim window would wait for the match to end.
			m_Session->TickAdmissionPlane(m_SessionPumpNowMs);
		}
		for (const NetTransportEvent& event: events) {
			m_Session->InjectEvent(event, m_SessionPumpNowMs);
		}
		if (hostAdmission) {
			// The reseat is a lockstep command, not a local mutation: every peer applies the identical
			// ownership at the identical tick. QueueLocalInput restamps the sender as this peer, which
			// on the host is exactly the id MovableMan's reseat gate requires.
			for (const NetGameReseat& reseat: m_ReconnectHost.TakePendingReseats()) {
				std::cout << "[net-reconnect] reseating team " << reseat.team << " onto peer "
				          << static_cast<int>(reseat.newOwnerPeerId) << " (" << reseat.actorUIDs.size() << " actors)" << std::endl;
				ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{ScenarioRunner::GetLockstepHostPeerId(), reseat});
			}
			m_SeatStatuses = m_ReconnectHost.GetSeatStatuses();
		}
		if (events.empty()) {
			return;
		}
		// A transport peer that reached session-Ready but carries no lockstep remote is a
		// reconnector: the host ends the round so everyone reconvenes around its snapshot.
		if (m_IsHost && m_ResyncOnDesync && m_Coordinator && m_Coordinator->IsRunning()) {
			for (const NetSessionPeerInfo& peer: m_Session->GetReadyPeers()) {
				if (!m_Coordinator->UsesTransportPeer(peer.transportPeerId)) {
					std::cout << "[net-match] rejoin: " << (peer.displayName.empty() ? "a player" : peer.displayName) << " reconnected - resyncing the match" << std::endl;
					m_Coordinator->RequestResync("player rejoined");
					break;
				}
			}
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
		// §11: a dropped seat stays marked on the roster for as long as it is dropped, so the remaining
		// players see who is missing rather than a toast they may have blinked past.
		for (NetLobbyMember& member: snapshot.members) {
			for (const NetH4SeatStatus& seat: m_SeatStatuses) {
				if (seat.lockstepPeerId == member.peerId) {
					member.dropped = seat.dropped;
					member.reclaiming = seat.reclaiming;
					break;
				}
			}
		}
		return snapshot;
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
			{"client_state", NetReconnectClientStateName(m_ReconnectClient.GetState())},
			{"client_used_stored_ticket", m_ReconnectClient.UsedStoredTicket()},
			{"client_commits", m_ReconnectClient.GetStats().commitsReceived},
			{"client_leave_acks", m_ReconnectClient.GetStats().leaveAcksReceived},
			{"client_ambiguous_losses", m_ReconnectClient.GetStats().ambiguousLosses},
			{"client_confirmed_session_ends", m_ReconnectClient.GetStats().confirmedSessionEnds},
		};
		json seats = json::array();
		for (const NetH4SeatStatus& seat: m_SeatStatuses) {
			seats.push_back({{"stable_seat", seat.stableSeat},
			                 {"lockstep_peer_id", static_cast<int>(seat.lockstepPeerId)},
			                 {"committed", seat.committed},
			                 {"closed", seat.closed},
			                 {"dropped", seat.dropped},
			                 {"reclaiming", seat.reclaiming}});
		}
		reconnect["seats"] = seats;
		report["reconnect"] = reconnect;
		if (m_Runner && m_Session && m_Coordinator) {
			report["runner"] = json::parse(m_Runner->BuildReportJson(*m_Session, *m_Coordinator));
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
		runnerConfig.publishLobby = [this](const NetLobbySnapshot& snapshot) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_LobbySnapshot = snapshot;
		};

		AttachAdmissionPlane(*session, request, runnerConfig.matchConfig, runnerConfig.sessionConfig, manifest);

		std::string error;
		bool started = runner->Start(*transport, *session, *coordinator, runnerConfig, &error);
		// A joiner whose lobby round carried a match state is RECONNECTING into a live match; it
		// launches from the received snapshot instead of a fresh activity. Launching a FRESH match
		// while the others play the snapshot would desync instantly, so a failed write fails the join.
		std::string pendingLoad;
		if (started) {
			std::vector<uint8_t> receivedState = runner->TakeReceivedState();
			if (!receivedState.empty()) {
				// Per-peer filename, matching the resync worker's convention.
				const std::string recvName = ResyncSaveName() + "_recv";
				const std::string recvPath = g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/" + recvName + ".ccsave";
				std::ofstream out(recvPath, std::ios::binary | std::ios::trunc);
				out.write(reinterpret_cast<const char*>(receivedState.data()), static_cast<std::streamsize>(receivedState.size()));
				out.close();
				if (out.good()) {
					pendingLoad = recvName;
				} else {
					error = "could not write the received match snapshot";
					started = false;
				}
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

	void NetMatchService::SetTicketStorePath(std::string path) {
		s_TicketStorePath = std::move(path);
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
