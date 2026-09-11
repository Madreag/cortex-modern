#pragma once

#include "NetLanDiscovery.h"
#include "NetLobbySnapshot.h"
#include "NetMatchRunner.h"
#include "NetReconnectSession.h"
#include "NetReconnectTicketStore.h"
#include "NetReconnectUx.h"
#include "NetSeatAuth.h"
#include "NetResyncState.h"
#include "Singleton.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#define g_NetMatchService NetMatchService::Instance()

namespace RTE {

	class Activity;
	class GnsTransport;

	enum class NetRejoinAnswer : uint8_t {
		Resync = 0,
		MatchOver = 1,
	};

	struct NetMatchE2ETickClock {
		uint64_t startTick = UINT64_MAX;
		uint64_t segmentTicks = 0;
		uint64_t priorTicks = 0;

		void NoteSimTick(uint64_t nowTick) {
			if (startTick == UINT64_MAX) {
				startTick = nowTick;
			}
			segmentTicks = nowTick - startTick;
		}
		void OnResyncRelaunch() {
			priorTicks += segmentTicks;
			startTick = UINT64_MAX;
			segmentTicks = 0;
		}
		void OnNewMatch() {
			priorTicks = 0;
			startTick = UINT64_MAX;
			segmentTicks = 0;
		}
		uint64_t Total() const { return priorTicks + segmentTicks; }
		bool EarlyOverIsSetupFailure() const { return Total() < 100; }
	};

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
		bool autoInputDelay = false; // Host: raise the delay to cover the measured peer RTT (the manual value stays the floor).
		uint8_t peerCount = 2; // Total players (2..4); the host listens for peerCount-1 clients.
		NetMatchMode mode = NetMatchMode::PvPSkirmish; // Shapes the roster: PvP (a team per peer), co-op PvE (one shared team vs CPU), PvPvE (teams + CPU).
		bool resyncOnDesync = false; // A runtime desync reloads everyone from the host's snapshot instead of aborting the match.
	};

	class NetMatchService : public Singleton<NetMatchService> {
	public:
		NetMatchService() = default;
		~NetMatchService();

		/// Turns the H4 admission plane off for a run. It is on by default; this exists so a two-peer
		/// gate can be bisected against the pre-admission handshake without a rebuild.
		static void SetAdmissionEnabled(bool enabled) { s_AdmissionEnabled = enabled; }
		static bool IsAdmissionEnabled() { return s_AdmissionEnabled; }
		/// §11: the multiprocess reconnect test shares one Userdata, so each process gets its own
		/// recovery-record path instead of racing over the default one.
		static void SetTicketStorePath(std::string path);
		/// Phase B, client: ask the host for this seat instead of joining one. The UI (B2) sets it from
		/// the roster; the gate drivers set it from the command line.
		static void SetApplyForSeat(bool enabled, uint16_t stableSeat);
		/// Phase B, host: stand in for the moderator in an unattended gate - approve the first
		/// applicant for this seat after the delay, and optionally withdraw the approval again.
		static void SetAutoSubstitute(bool enabled, uint16_t stableSeat, uint64_t delayMs, bool thenCancel);

		bool Start(const NetMatchServiceRequest& request, std::string* error = nullptr);
		bool CanSealA7Journal() const;

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
		/// "player left" (2-peer); the session objects stay alive exactly like FinishMatch. §7's leave
		/// exchange runs first, on the worker, so the ticket is answered while the link is still up.
		void LeaveMatch(const std::string& result);
		/// Blocks until the worker has finished. A report written before a leave settles would describe
		/// the exchange as unacknowledged when it was not.
		void WaitForPendingWork();

		bool ConsumeReadyToLaunch(std::string& outActivityPreset);

		/// Runs the mid-match session upkeep: drains the reconnect-handshake events the coordinator
		/// handed over, and (host) turns a newly Ready session peer into a resync-for-rejoin.
		void PumpSessionEvents();
		/// Consumes the host's current seat snapshot on the game thread.
		void PumpSeatPresence();
		/// The reconnect UX state machine (§11): auto-retry, the stored-ticket offer and the roster's
		/// dropped/reclaiming marks. Game-thread only.
		NetReconnectUx& GetReconnectUx() { return m_ReconnectUx; }
		const NetReconnectUx& GetReconnectUx() const { return m_ReconnectUx; }
		/// Reads the cached moderation view; actions require a running match on the game thread.
		std::vector<NetH4ModerationSeat> GetModerationSeats() const;
		NetH4ModerationResult ApplyModeration(const NetModerationSelection& selection, NetModerationAction action);

		/// Re-enters the match this process was dropped from, using the stored recovery record.
		bool BeginTicketRejoin(std::string* error = nullptr);
		/// §11: reads the recovery record so the landing screen can offer a rejoin after a relaunch, or
		/// say exactly why it cannot. Read-only and safe to call repeatedly.
		void ScanStoredTicket();

		NetMatchServiceState GetState() const;
		bool WasEverStarted() const { return m_EverStarted.load(); }
		NetLobbySnapshot GetLobbySnapshot() const;
		std::string GetStatusText() const;
		std::string GetErrorText() const;
		std::string BuildReportJson() const;
		uint8_t GetLocalPeerId() const;
		int GetLocalTeam() const;

		static bool ResyncSnapshotAllowed(const Activity* activity);
		static NetRejoinAnswer ClassifyRejoin(const Activity* activity);
		void AnswerMatchOverRejoin(const std::string& result);

		static const char* StateName(NetMatchServiceState state);

	private:
		void WorkerMain(NetMatchServiceRequest request, NetIdentityManifest manifest);
		void WorkerRematchMain(GnsTransport* transportRaw, NetSession* sessionRaw, NetLockstepCoordinator* coordinatorRaw, NetMatchRunner* runnerRaw);
		void WorkerResyncMain(GnsTransport* transportRaw, NetSession* sessionRaw, NetLockstepCoordinator* coordinatorRaw, NetMatchRunner* runnerRaw, std::vector<uint8_t> stateBytes);
		bool PrepareReceivedResync(const std::vector<uint8_t>& bytes, const NetLockstepCoordinator& coordinator, std::string& pendingLoad, NetResyncState& state, std::string* error);
		NetSessionConfig BuildSessionConfig(const NetIdentityManifest& manifest, const NetMatchServiceRequest& request) const;
		NetMatchConfig BuildMatchConfig(const NetMatchServiceRequest& request, uint64_t sessionId) const;
		void SetState(NetMatchServiceState state, std::string status, std::string error = "");
		void JoinWorkerIfDone();
		/// Attaches the H4 admission plane to a freshly built session. Host: only with a live auth
		/// epoch, so a build without crypto keeps the pre-admission handshake and issues no tickets.
		void AttachAdmissionPlane(NetSession& session, const NetMatchServiceRequest& request, const NetMatchConfig& matchConfig, const NetSessionConfig& sessionConfig, const NetIdentityManifest& manifest);
		/// The drop-frame ownership census. Called by the reconnect host, and only ever from inside
		/// PumpSessionEvents on the game thread - g_MovableMan is not safe to walk from anywhere else.
		static std::vector<NetH4LedgerActor> CollectDropOwnership(void* context);
		/// The H4 seat state the round consults before it adjudicates a lost transport. Called by the
		/// coordinator on the game thread, which never holds this lock.
		static NetLockstepSeatState QuerySeatState(void* context, uint8_t lockstepPeerId, NetPeerId transportPeerId);
		/// Client: the §7 leave protocol, waiting exactly P21's budget for the ack before giving up and
		/// KEEPING the ticket. Runs only with a plane attached and a record to lose.
		void RunCleanLeave();
		/// The worker half of a leave: the §7 exchange, then - and only then - the round is told.
		void LeaveWorkerMain(std::string result);
		/// Ends the hosted session: tells every peer with the one reason that permits deleting a
		/// recovery record (P22), then clears the registry, the ledger and the seats. Caller holds the lock.
		void EndAdmissionSession();
		/// Runs the §11 automatic-retry schedule from the service's own state. Game thread only.
		void DriveReconnectUx(uint64_t nowMs);
		/// Elapsed milliseconds since this session began, for every admission deadline.
		uint64_t AdmissionNowMs() const;
		void CaptureA7SeatView();
		bool WaitForA7ConnectGate(std::string* error);


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
		std::optional<NetResyncState> m_PendingResyncState;
		bool m_ResyncRetainsLocalState = false;
		uint64_t m_ResyncSourceRound = 0;
		std::string m_LocalName;
		NetLobbySnapshot m_LobbySnapshot;
		NetSeatAuthRegistry m_SeatAuth; //!< Hosted-session reconnect-auth material (off-sim epoch + seat credentials); survives resync/rejoin/rematch.
		// The admission plane lives on the service, not on a session or a match round, so a seat and its
		// ledger survive resync, rejoin and rematch exactly as the registry does (§3).
		NetReconnectHost m_ReconnectHost;
		NetReconnectClient m_ReconnectClient;
		NetReconnectTicketStore m_TicketStore;
		NetReconnectUx m_ReconnectUx;
		NetSeatPresence m_SeatPresence;
		std::vector<NetH4ModerationSeat> m_ModerationSeats; //!< Immutable UI copy while a setup/resync worker owns the plane.
		bool m_AdmissionAttached = false;
		bool m_LeaveExchangeRun = false; //!< The §7 exchange has been attempted for this session; Destroy must not repeat it.
		bool m_MatchWasRunning = false;  //!< This session reached a running match, so §11's recovery applies to losing it.
		std::vector<NetH4SeatStatus> m_SeatStatuses; //!< Published from the sim pump for the roster (§11).
		std::atomic<uint32_t> m_CensusRefusals{0};   //!< Ownership censuses refused because the caller was not the sim thread.
		/// The largest the session clock has ever run ahead of the admission clock at a pump. Zero on a
		/// tree where they are one clock; the inflation itself on one where they are not, whenever the
		/// report is written - which the two clocks in the report cannot say, being read after teardown.
		std::atomic<uint64_t> m_MaxClockDivergenceMs{0};
		/// The moderator stand-in for the unattended gates: runs from PumpSessionEvents, on the game
		/// thread, and does exactly what a host clicking the UI would do.
		void DriveAutoSubstitution(uint64_t nowMs);
		/// Called with the service lock on the game thread, when it owns the admission plane.
		void PublishModerationView();

		static bool s_AdmissionEnabled;
		static std::string s_TicketStorePath;
		static bool s_ApplyForSeat;
		static uint16_t s_ApplySeat;
		static bool s_AutoSubstitute;
		static uint16_t s_AutoSubstituteSeat;
		static uint64_t s_AutoSubstituteDelayMs;
		static bool s_AutoSubstituteThenCancel;
		uint64_t m_AutoSubstituteReadyMs = 0; //!< When the stand-in first saw an applicant it could approve.
		bool m_AutoSubstituteDone = false;

		std::unique_ptr<GnsTransport> m_Transport;
		std::unique_ptr<NetSession> m_Session;
		std::unique_ptr<NetLockstepCoordinator> m_Coordinator;
		std::unique_ptr<NetMatchRunner> m_Runner;
		std::vector<NetTransportEvent> m_PendingSessionEvents; //!< Game-thread only: reconnect traffic the coordinator handed over.
		NetAdmissionClock m_AdmissionClock; //!< One elapsed-time source for setup, play, stalls and resync.
		NetLanDiscovery m_LanDiscovery; //!< Game-thread only: the hosting lobby's LAN beacon.
		uint16_t m_BeaconGamePort = 0;
		uint8_t m_BeaconMaxPlayers = 2;
		std::atomic<bool> m_ReadyRequested{false};
		std::atomic<bool> m_StartRequested{false};
		std::atomic<bool> m_CancelRequested{false};
		std::atomic<bool> m_EverStarted{false};
		std::string m_CapturedRunnerReport;
		std::string m_RejoinOutcome;
	};

} // namespace RTE
