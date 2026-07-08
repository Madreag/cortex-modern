#pragma once

#include "NetLanDiscovery.h"
#include "NetLobbySnapshot.h"
#include "NetMatchRunner.h"
#include "Singleton.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#define g_NetMatchService NetMatchService::Instance()

namespace RTE {

	class GnsTransport;

	enum class NetMatchServiceState {
		Idle,
		Starting,
		ReadyToLaunch,
		Running,
		Completed,
		Failed,
	};

	struct NetMatchServiceRequest {
		bool host = false;
		std::string address = "127.0.0.1";
		uint16_t port = 41010;
		std::string playerName = "Player";
		std::string activityPreset = "Skirmish Defense";
		NetActorOwnershipPolicy ownershipPolicy = NetActorOwnershipPolicy::TeamOwner;
		uint16_t inputDelayFrames = 0; // Lockstep input-delay buffer; the host picks it, the client agrees at the start handshake.
		uint8_t peerCount = 2; // Total players (2..4); the host listens for peerCount-1 clients.
		NetMatchMode mode = NetMatchMode::PvPSkirmish; // Shapes the roster: PvP (a team per peer), co-op PvE (one shared team vs CPU), PvPvE (teams + CPU).
		bool resyncOnDesync = false; // A runtime desync reloads everyone from the host's snapshot instead of aborting the match.
	};

	class NetMatchService : public Singleton<NetMatchService> {
	public:
		NetMatchService() = default;
		~NetMatchService();

		bool Start(const NetMatchServiceRequest& request, std::string* error = nullptr);

		/// Reconvenes a completed match's still-connected session in the lobby for a rematch.
		/// Fails (and settles the service into Failed) when the session was lost.
		bool ReturnToLobby(std::string* error = nullptr);

		/// Recovers a desynced match: the host snapshots its state and streams it through the lobby
		/// round; every peer relaunches from the identical file. Requires the session to be alive.
		bool ResyncMatch(std::string* error = nullptr);
		bool IsResyncOnDesyncEnabled() const { return m_ResyncOnDesync; }
		/// The snapshot file the next launch must load instead of a fresh activity ("" = none).
		std::string TakePendingResyncLoad();
		bool HasPendingResyncLoad() const;
		/// Stages the pending snapshot for launch: the world state is the file's, the player seats
		/// are per-peer, and the funds/roster ride the snapshot untouched.
		bool StageResyncedMatchLaunch(std::string* error = nullptr);
		void Destroy();
		void Update();
		void SetReady();
		void RequestStart();
		void ReportRuntimeError(const std::string& error);
		void Complete(const std::string& reason);
		void FinishMatch(const std::string& result);
		/// Ends the match locally as a clean leave: the other peers keep playing (N-peer) or hear
		/// "player left" (2-peer); the session objects stay alive exactly like FinishMatch.
		void LeaveMatch(const std::string& result);

		bool ConsumeReadyToLaunch(std::string& outActivityPreset);

		/// Runs the mid-match session upkeep: drains the reconnect-handshake events the coordinator
		/// handed over, and (host) turns a newly Ready session peer into a resync-for-rejoin.
		void PumpSessionEvents();
		NetMatchServiceState GetState() const;
		bool WasEverStarted() const { return m_EverStarted.load(); }
		NetLobbySnapshot GetLobbySnapshot() const;
		std::string GetStatusText() const;
		std::string GetErrorText() const;
		std::string BuildReportJson() const;
		uint8_t GetLocalPeerId() const;
		int GetLocalTeam() const;

		static const char* StateName(NetMatchServiceState state);

	private:
		void WorkerMain(NetMatchServiceRequest request, NetIdentityManifest manifest);
		void WorkerRematchMain(GnsTransport* transportRaw, NetSession* sessionRaw, NetLockstepCoordinator* coordinatorRaw, NetMatchRunner* runnerRaw);
		void WorkerResyncMain(GnsTransport* transportRaw, NetSession* sessionRaw, NetLockstepCoordinator* coordinatorRaw, NetMatchRunner* runnerRaw, std::vector<uint8_t> stateBytes);
		NetSessionConfig BuildSessionConfig(const NetIdentityManifest& manifest, const NetMatchServiceRequest& request) const;
		NetMatchConfig BuildMatchConfig(const NetMatchServiceRequest& request, uint64_t sessionId) const;
		void SetState(NetMatchServiceState state, std::string status, std::string error = "");
		void JoinWorkerIfDone();

		mutable std::mutex m_Mutex;
		NetMatchServiceState m_State = NetMatchServiceState::Idle;
		std::string m_StatusText = "Idle";
		std::string m_ErrorText;
		std::string m_ActivityPreset;
		std::thread m_Worker;
		bool m_WorkerDone = false;
		bool m_IsHost = false;
		uint8_t m_LocalPeerId = 0;
		int m_LocalTeam = -1;
		bool m_ResyncOnDesync = false;
		std::string m_PendingResyncLoad;
		std::string m_LocalName;
		NetLobbySnapshot m_LobbySnapshot;

		std::unique_ptr<GnsTransport> m_Transport;
		std::unique_ptr<NetSession> m_Session;
		std::unique_ptr<NetLockstepCoordinator> m_Coordinator;
		std::unique_ptr<NetMatchRunner> m_Runner;
		std::vector<NetTransportEvent> m_PendingSessionEvents; //!< Game-thread only: reconnect traffic the coordinator handed over.
		uint64_t m_SessionPumpNowMs = 0;
		NetLanDiscovery m_LanDiscovery; //!< Game-thread only: the hosting lobby's LAN beacon.
		uint16_t m_BeaconGamePort = 0;
		uint8_t m_BeaconMaxPlayers = 2;
		std::atomic<bool> m_ReadyRequested{false};
		std::atomic<bool> m_StartRequested{false};
		std::atomic<bool> m_CancelRequested{false};
		std::atomic<bool> m_EverStarted{false};
	};

} // namespace RTE
