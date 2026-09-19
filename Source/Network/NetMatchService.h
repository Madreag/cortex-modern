#pragma once

#include "NetDirectoryClient.h"
#include "NetLanDiscovery.h"
#include "NetLobbySnapshot.h"
#include "NetMatchRunner.h"
#include "NetMuxTransport.h"
#include "NetReconnectSession.h"
#include "NetReconnectTicketStore.h"
#include "NetReconnectUx.h"
#include "NetSeatAuth.h"
#include "NetResyncState.h"
#include "Singleton.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define g_NetMatchService NetMatchService::Instance()

namespace RTE {

	class Activity;
	class GnsDirectorySignalDispatcher;
	class GnsTransport;

	enum class NetRejoinAnswer : uint8_t {
		Resync = 0,
		MatchOver = 1,
	};

	struct NetMatchE2ETickClock {
		uint64_t firstTick = UINT64_MAX;
		uint64_t lastTick = UINT64_MAX;
		uint64_t priorTicks = 0;
		uint64_t segmentFirstFrame = 0; // The frame this segment resumed at; 0 when only the observed ticks are known.

		void NoteSimTick(uint64_t nowTick) {
			if (firstTick == UINT64_MAX) {
				firstTick = nowTick;
			}
			lastTick = nowTick;
		}
		uint64_t SegmentTicks() const {
			if (firstTick == UINT64_MAX) {
				return 0;
			}
			const uint64_t origin = segmentFirstFrame > 0 ? segmentFirstFrame : firstTick;
			return lastTick >= origin ? lastTick - origin + 1 : 0;
		}
		// The healed round replays from resumeFrame, so the frames before it are the round's and are
		// counted once: the clock stays the round's frame number whatever the relaunch cost each peer.
		void OnResyncRelaunch(uint64_t resumeFrame = 0) {
			priorTicks = resumeFrame > 0 ? resumeFrame - 1 : priorTicks + SegmentTicks();
			segmentFirstFrame = resumeFrame;
			firstTick = UINT64_MAX;
			lastTick = UINT64_MAX;
		}
		void OnNewMatch() {
			priorTicks = 0;
			segmentFirstFrame = 0;
			firstTick = UINT64_MAX;
			lastTick = UINT64_MAX;
		}
		uint64_t Total() const { return priorTicks + SegmentTicks(); }
		bool EarlyOverIsSetupFailure() const { return Total() < 100; }
		bool EarlyOverIsSetupFailure(uint64_t matchTick) const { return matchTick < 100; }
	};

	inline uint64_t ParseLockstepStopTick(const std::string& error, uint64_t fallbackTick) {
		if (error.size() < 6 || error.compare(0, 5, "tick ") != 0) {
			return fallbackTick;
		}
		uint64_t tick = 0;
		bool any = false;
		for (size_t i = 5; i < error.size(); ++i) {
			const char c = error[i];
			if (c < '0' || c > '9') {
				break;
			}
			any = true;
			tick = tick * 10 + static_cast<uint64_t>(c - '0');
		}
		return any ? tick : fallbackTick;
	}

	// Cap completion is the match frame, not how long this process has run.
	inline bool NetMatchE2EReachedCap(uint64_t runningTicks, uint64_t matchTick, uint64_t cap) {
		(void)runningTicks;
		return matchTick >= cap;
	}

	// The stop a peer sends once the round has run its planned length.
	inline constexpr const char* c_NetMatchE2ECompleteStop = "Complete:e2e complete";

	/// Whether an e2e round's stop is the round reaching its planned end rather than a break.
	inline bool NetMatchE2ERoundReachedPlannedEnd(const std::string& error, uint64_t runningTicks, uint64_t matchTick, uint64_t cap) {
		// This stop originates from the peer that ran the round to its plan, so it ends the round on
		// every peer whatever tick the local sim is on when it lands.
		if (error.find(c_NetMatchE2ECompleteStop) != std::string::npos) {
			return true;
		}
		return NetMatchE2EReachedCap(runningTicks, matchTick, cap) &&
		       (error.find("Complete:") != std::string::npos ||
		        error.find("MissingFrameTimeout") != std::string::npos ||
		        error.find("PeerDisconnected") != std::string::npos);
	}

	/// Where a session-id join has to dial, once a directory row has been resolved.
	struct NetIceJoinTarget {
		std::string identity;  //!< The host's GNS identity; empty on an ip-only row.
		std::string joinMode;  //!< "ip" | "ice" | "either", as the row carries it.
		std::string address;   //!< Set when the row also advertises a direct address.
		uint16_t port = 0;
	};

	/// The GNS identity a host binds for a directory session; the dispatcher's rule, readable in a
	/// build without GameNetworkingSockets.
	std::string NetIceHostIdentity(const std::string& sessionId);

	/// Reports ICE reachability only for the directory id bound to the listener.
	std::string NetIceRowJoinMode(bool iceEnabled, bool hasDirectAddress, const std::string& boundSessionId, const std::string& rowSessionId);

	/// Resolves a session id against a directory listing. Empty and a filled target when the row can
	/// be joined, else the join list's own refusal label for it.
	std::string NetIceResolveSessionRow(const std::vector<NetDirectorySessionRow>& rows, const NetDirectoryLocalIdentity& local, const std::string& sessionId, NetIceJoinTarget* out);

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
		std::string activityModule; // The module that defines the preset; empty resolves to the module defining it.
		std::optional<NetMatchStandardRules> standardRules;
		NetActorOwnershipPolicy ownershipPolicy = NetActorOwnershipPolicy::TeamOwner;
		uint16_t inputDelayFrames = 0; // Lockstep input-delay buffer; the host picks it, the client agrees at the start handshake.
		bool autoInputDelay = false; // Host: raise the delay to cover the measured peer RTT (the manual value stays the floor).
		uint8_t peerCount = 2; // Connected peers (2..4), including a dedicated host.
		std::optional<uint32_t> humans; // Omitted seats every available human peer.
		std::optional<uint32_t> cpuSlots; // Omitted keeps the mode's default CPU count.
		NetMatchMode mode = NetMatchMode::PvPSkirmish; // Shapes the roster: PvP (a team per peer), co-op PvE (one shared team vs CPU), PvPvE (teams + CPU).
		std::optional<bool> brainlessHumansSpectate; // Host rule: the round survives the last human brain. Unset takes the host's Gameplay setting.
		// The host's saved session options. Unset keeps the config default; NetMatchService::SeatSavedOptions fills them from the settings.
		std::optional<NetMatchDelayPolicy> delayPolicy;
		std::optional<uint8_t> idleWaitMinutes;
		std::optional<bool> automaticRepair;
		bool resyncOnDesync = false; // A runtime desync reloads everyone from the host's snapshot instead of aborting the match.
		bool dedicated = false; // Host only: keep lockstep peer hostPeerId but seat no human slot there.
		std::string sessionId; // Client only: join the directory session with this id instead of an address.
	};

	inline NetMatchServiceRequest TicketRejoinRequestFromRecord(const NetH4TicketRecord& record, const std::string& playerName) {
		NetMatchServiceRequest request;
		request.host = false;
		request.address = record.hostAddress;
		request.sessionId = record.directorySessionId;
		request.playerName = playerName.empty() ? "Client" : playerName;
		request.resyncOnDesync = true;
		return request;
	}

	inline std::string ResolveTicketJoinAddress(const NetH4TicketRecord& record, const std::string& requestSessionId, const std::string& requestAddress, const std::string& directoryResolvedAddress, bool iceDial) {
		const std::string sessionId = !record.directorySessionId.empty() ? record.directorySessionId : requestSessionId;
		if (!directoryResolvedAddress.empty()) {
			return directoryResolvedAddress;
		}
		if (!sessionId.empty() && iceDial) {
			return "session:" + sessionId;
		}
		if (!record.hostAddress.empty()) {
			return record.hostAddress;
		}
		if (!sessionId.empty()) {
			return "session:" + sessionId;
		}
		return requestAddress;
	}

	/// Polls a configured directory client until it answers a list or the budget runs out; the rows it
	/// returns are what a rejoin re-resolves against.
	std::vector<NetDirectorySessionRow> BrowseSessionRows(NetDirectoryClient& browse, uint64_t budgetMs, const std::function<bool()>& cancelled);

	/// A stored ticket belongs to this join only when it names the host this request dials or the session
	/// it joins; a record left by another host is not a re-resolve of this one.
	inline bool TicketMatchesRequest(const NetH4TicketRecord& record, const std::string& requestSessionId, const std::string& requestAddress) {
		if (!record.directorySessionId.empty() && record.directorySessionId == requestSessionId) {
			return true;
		}
		return !record.hostAddress.empty() && record.hostAddress == requestAddress;
	}

	/// The address a ticket rejoin dials: the row the directory browse found for the stored session, else
	/// the ticket's own address or session id.
	inline std::string ResolveTicketJoinAddressFromRows(const NetH4TicketRecord& record, const std::string& requestSessionId, const std::string& requestAddress, const std::vector<NetDirectorySessionRow>& rows, const NetDirectoryLocalIdentity& local, bool iceDial) {
		const std::string sessionId = !record.directorySessionId.empty() ? record.directorySessionId : requestSessionId;
		std::string resolved;
		if (!sessionId.empty() && !rows.empty()) {
			NetIceJoinTarget target;
			if (NetIceResolveSessionRow(rows, local, sessionId, &target).empty() && !target.address.empty()) {
				resolved = target.address;
			}
		}
		return ResolveTicketJoinAddress(record, requestSessionId, requestAddress, resolved, iceDial);
	}

	/// A presentation-only record of the finished round; never restored into the simulation.
	struct NetMatchSummary {
		struct Peer {
			uint8_t peerId = 0;
			std::string name;
			int team = -1;
			uint16_t seat = 0;
			uint16_t inputDelayFrames = 0;
		};
		std::string result;
		int winnerTeam = -1;
		uint64_t runningTicks = 0;
		std::vector<Peer> peers;
		uint32_t resyncs = 0;
		uint32_t drops = 0;
		uint32_t reclaims = 0;
		uint32_t substitutions = 0;
		std::string paceJson = "{}";
		std::string identityLine;
		/// Formats the recorded duration at 60 ticks per second.
		std::string DurationText() const;
		/// Formats the single lobby line and the complete dialog body.
		std::string LineText() const;
		std::string DetailsText() const;
		/// Formats the identity, reading and caching the executable hash on first use.
		std::string IdentityText() const;
	};

	class NetMatchService : public Singleton<NetMatchService> {
	public:
		// Defined in the .cpp: the dispatcher member is only a declaration in this header.
		NetMatchService();
		~NetMatchService();

		/// Turns the H4 admission plane off for a run. It is on by default; this exists so a two-peer
		/// gate can be bisected against the pre-admission handshake without a rebuild.
		static void SetAdmissionEnabled(bool enabled) { s_AdmissionEnabled = enabled; }
		static bool IsAdmissionEnabled() { return s_AdmissionEnabled; }
		/// Overrides this run's checkpoint cadence in simulation seconds; zero disables it.
		static void SetAutosaveSeconds(uint32_t seconds) {
			s_AutosaveSeconds = seconds;
			s_AutosaveSecondsOverridden = true;
		}
		/// Applies the saved cadence while preserving any command-line override.
		static void SetAutosaveSecondsSetting(uint32_t seconds) {
			if (!s_AutosaveSecondsOverridden) s_AutosaveSeconds = seconds;
		}
		/// Gets this run's checkpoint cadence, including its command-line override.
		static uint32_t GetAutosaveSeconds() { return s_AutosaveSeconds; }
		static constexpr uint32_t c_MaxAutosaveIntervalSeconds = 3600; // An hour is the longest cadence a host may announce.
		/// The cadence a running match keeps: the command-line override when one was given, else the host's announced option.
		static uint32_t MatchAutosaveSeconds(const NetMatchConfig& config) {
			if (s_AutosaveSecondsOverridden) return s_AutosaveSeconds;
			return config.autosaveEnabled ? config.autosaveIntervalSeconds : 0;
		}
		/// Runs only after a complete lockstep tick, outside paused ticks and preview frames.
		void AutosaveAtTickBoundary(uint64_t tick);
		/// §11: the multiprocess reconnect test shares one Userdata, so each process gets its own
		/// recovery-record path instead of racing over the default one.
		static void SetTicketStorePath(std::string path);
		/// Phase B, client: ask the host for this seat instead of joining one. The UI (B2) sets it from
		/// the roster; the gate drivers set it from the command line.
		static void SetApplyForSeat(bool enabled, uint16_t stableSeat);
		/// Phase B, host: stand in for the moderator in an unattended gate - approve the first
		/// applicant for this seat after the delay, and optionally withdraw the approval again.
		static void SetAutoSubstitute(bool enabled, uint16_t stableSeat, uint64_t delayMs, bool thenCancel);
		/// A joiner finishes its startup and then waits for this file before it connects, so a gate can
		/// place a second holder of one ticket at a chosen moment of the match instead of at boot time.
		static void SetJoinWaitPath(std::string path);
		/// Polls `path` every 100 ms until it exists. False (with `error`) when `budgetMs` runs out.
		static bool WaitForJoinTrigger(const std::string& path, uint64_t budgetMs, std::string* error);
		static constexpr uint64_t c_JoinWaitBudgetMs = 120000;
		static constexpr uint64_t c_JoinWaitPollMs = 100;
		/// How long a finished match's rematch lobby waits for every peer to come back before the
		/// service destroys it and releases the session, the seats and the directory lease.
		static constexpr uint64_t c_CompletedLobbyExpiryMs = 600000;

		bool Start(const NetMatchServiceRequest& request, std::string* error = nullptr);
		bool CanSealA7Journal() const;

		/// Reconvenes a completed match's still-connected session in the lobby for a rematch.
		/// Fails (and settles the service into Failed) when the session was lost.
		bool ReturnToLobby(std::string* error = nullptr);

		/// Recovers a desynced match: the host snapshots its state and streams it through the lobby
		/// round; every peer relaunches from the identical file. Requires the session to be alive.
		bool ResyncMatch(std::string* error = nullptr);
		void NoteResyncRelaunched();
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
		/// The seat-presence plane — where dropped seats get their reclaim-hold marks.
		const NetSeatPresence& GetSeatPresence() const { return m_SeatPresence; }
		NetH4ModerationResult ApplyModeration(const NetModerationSelection& selection, NetModerationAction action);

		/// Re-enters the match this process was dropped from, using the stored recovery record.
		bool BeginTicketRejoin(std::string* error = nullptr);
		/// §11: reads the recovery record so the landing screen can offer a rejoin after a relaunch, or
		/// say exactly why it cannot. Read-only and safe to call repeatedly.
		void ScanStoredTicket();
		/// Whether the §11 retry schedule still has work, so the menu loop pumps the service whatever
		/// screen is up rather than only while the multiplayer screen is open.
		bool NeedsRecoveryPump() const;
		/// Whether a finished match still wants the menu loop's pump for its rematch lobby and kept
		/// directory lease. Not a recovery: the screens route a drop, not an ordinary match end.
		bool NeedsCompletedLobbyPump() const;
		/// Whether the host refused the last join because its match is already running, which is the
		/// only case §9b's applicant path exists for.
		bool WasJoinRefusedByALiveMatch() const;
		/// Asks the host for a seat instead of joining one: the same connection the join used, with
		/// §9b's application in place of the new-join request. The host picks the seat.
		bool BeginSubstituteApplication(const NetMatchServiceRequest& request, std::string* error = nullptr);

		NetMatchServiceState GetState() const;
		bool IsHost() const { std::lock_guard<std::mutex> lock(m_Mutex); return m_IsHost; }
		bool WasEverStarted() const { return m_EverStarted.load(); }
		/// The host's router port-mapping state, for the lobby's status line. Game-thread only.
		struct PortMapStatus {
			bool enabled = false;    //!< This match's host asked the router for a mapping.
			bool done = false;       //!< The request settled: mapped, or the chain gave up.
			bool mapped = false;     //!< A mapping is held right now.
			std::string method;      //!< "natpmp"|"pcp"|"upnp" while mapped.
			std::string externalIp;
			uint16_t externalPort = 0;
			std::string error;       //!< Why the chain gave up; empty while running or mapped.
		};
		PortMapStatus GetPortMapStatus() const;
		NetLobbySnapshot GetLobbySnapshot() const;
		/// Returns a copy that survives returning to the lobby and expires at the next match start.
		std::optional<NetMatchSummary> GetLastMatchSummary() const;
		/// Local chat send, presentation only. Reaches the session whether the lobby is still running
		/// on the worker or the match has handed it back; false when no session link exists.
		bool SendChat(uint8_t scope, const std::string& text);
		/// Drains the session's chat queue for the UI. Newest 64 are kept on the session side.
		std::vector<NetChatEntry> TakeChatEntries();
		std::vector<NetChatEntry> ChatHistory() const;
		/// "Input delay: N (auto, Rms ping)" / "(fixed)", from the announced match config. "" pre-lobby.
		std::string GetInputDelayText() const;
		/// The live host RTT on a client, or the largest connected peer RTT on the host.
		std::optional<uint32_t> GetMatchPingMs() const;
		/// Whether the current match is being restored from the host snapshot.
		bool IsMatchResyncing() const;
		/// The current seat holder's display name for presentation events.
		std::string GetPeerDisplayName(uint8_t peerId) const;
		std::string GetStatusText() const;
		std::string GetErrorText() const;
		std::string BuildReportJson() const;
		/// Builds the match roster from the request alone; it reads no manager, so a self-test can build one.
		static bool BuildMatchConfig(const NetMatchServiceRequest& request, uint64_t sessionId, NetMatchConfig& outConfig, std::string* error = nullptr);
		/// The module a module-less activity preset belongs to, from the modules that define it. Reads no
		/// manager: the caller lists the candidates.
		static bool ResolveActivityModule(const std::string& preset, const std::vector<std::string>& definingModules, std::string& outModule, std::string* error = nullptr);
		/// Fills an unset request module with the loaded module that defines the preset.
		static bool SeatActivityModule(NetMatchServiceRequest& request, std::string* error = nullptr);
		/// Fills the request's unset options from the saved settings, where a real host starts a match.
		static void SeatSavedOptions(NetMatchServiceRequest& request);
		/// Builds diagnostic identity on request; match startup supplies the cached join inputs.
		bool RefreshDiagnosticIdentity(std::string* error = nullptr, double* buildMs = nullptr);
		/// Returns the cached join inputs without reading settings, modules, or simulation state.
		std::string ExportDiagnosticIdentity() const;
		/// Returns the last runtime error and heal record without exposing reconnect credentials.
		std::string ExportDiagnosticDesyncHeal() const;
		uint8_t GetLocalPeerId() const;
		int GetLocalTeam() const;

		static bool ResyncSnapshotAllowed(const Activity* activity);
		static NetRejoinAnswer ClassifyRejoin(const Activity* activity);
		void AnswerMatchOverRejoin(const std::string& result);

		static const char* StateName(NetMatchServiceState state);

	private:
		/// A match's transports, moved as one into a rematch or resync worker and back.
		struct TransportLink {
			// Defined in the .cpp, where the dispatcher type is complete.
			TransportLink();
			TransportLink(TransportLink&&) noexcept;
			TransportLink& operator=(TransportLink&&) noexcept;
			~TransportLink();

			std::unique_ptr<GnsTransport> ip;
			std::unique_ptr<NetMuxTransport> mux;
			std::vector<NetTransportEvent> lobbyEvents;
#ifdef CCCP_WITH_GNS
			std::unique_ptr<GnsDirectorySignalDispatcher> dispatcher; //!< After the mux, so it is destroyed first.
#endif
			/// The session's wire: the mux if there is one, else the IP transport.
			INetTransport* Wire() const { return mux ? static_cast<INetTransport*>(mux.get()) : ip.get(); }
		};

		void WorkerMain(NetMatchServiceRequest request, NetIdentityManifest manifest);
		void WorkerRematchMain(TransportLink link, NetSession* sessionRaw, NetLockstepCoordinator* coordinatorRaw, NetMatchRunner* runnerRaw);
		void WorkerResyncMain(TransportLink link, NetSession* sessionRaw, NetLockstepCoordinator* coordinatorRaw, NetMatchRunner* runnerRaw, std::vector<uint8_t> stateBytes);
		/// The live wire, by the same rule. Caller holds the lock.
		INetTransport* ActiveWireLocked() const { return m_Mux ? static_cast<INetTransport*>(m_Mux.get()) : m_Transport.get(); }
		/// Hands the transports and dispatcher to a worker, caching the dispatcher's report. Caller holds the lock.
		TransportLink TakeTransportLinkLocked();
		/// Takes them back from a worker. Caller holds the lock.
		void RestoreTransportLinkLocked(TransportLink link);
		/// Refuses a resync with no live match or a lost session; a lost session fails the service. Caller holds the lock.
		bool CanResyncLocked(std::string* error);
		bool PrepareReceivedResync(const std::vector<uint8_t>& bytes, const NetLockstepCoordinator& coordinator, std::string& pendingLoad, NetResyncState& state, std::string* error, size_t* archiveBytes = nullptr);
		/// The session the round is hosted on; the adopted match config carries the seats it offers.
		NetSessionConfig BuildSessionConfig(const NetIdentityManifest& manifest, const NetMatchServiceRequest& request, const NetMatchConfig& matchConfig) const;
		void SetState(NetMatchServiceState state, std::string status, std::string error = "");
		/// Match end or the host leaving takes the directory row down now rather than at Destroy.
		/// Game-thread only, like the client it drives.
		void RetractDirectoryListing();
		/// Keeps only the registered row bound to this host's ICE identity. Game-thread only.
		bool ShouldKeepIceDirectoryLease() const;
		/// Hides the bound row while retaining its lease. Game-thread only.
		void HideDirectoryListing();
		/// Relists an acknowledged hidden lease or retracts a lost one. Game-thread only.
		void SettleKeptDirectoryLease();
		/// Host: waits for the register reply so the GNS identity can be pinned to the session id
		/// before any listen socket of this process opens. Worker thread; reads the published snapshot.
		bool WaitForDirectorySession(uint64_t budgetMs, std::string& sessionId, std::string& token) const;
		/// Host: registers first, pins the GNS identity to the session id, then opens both listens.
		/// Client: resolves the session id to a row and arms the join. Worker thread.
		bool SetUpIceTransport(const NetMatchServiceRequest& request, const NetIdentityManifest& manifest, NetMuxTransport& mux, NetSessionConfig& sessionConfig, std::string& joinAddress, std::string* error);
		/// The ICE virtual port a host listens on and a joiner dials.
		static constexpr int c_IceVirtualPort = 41011;
		static constexpr uint64_t c_IceRegisterBudgetMs = 30000;
		static constexpr uint64_t c_IceResolveBudgetMs = 30000;
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
		friend bool TestHoldResolutionPumpDoesNotRelock(std::string* error);
		friend bool TestMatchOverRejoinFromWaitKeepsCoordinator(std::string* error);
		friend bool TestRosterTransitionsRecordHoldThenPresent(std::string* error);
		friend bool TestRosterBannerNamesThePlayerOnce(std::string* error);
		friend bool TestAiOnlyHostSeatsNoJoiner(std::string* error);
		friend bool TestPendingSessionEventSurvivesTeardown(std::string* error);
		friend bool TestServiceReturnToLobbyFormsTheNextRoster(std::string* error);
		friend bool ServiceRematchRoster(NetMatchService& service, const NetMatchConfig& played, uint8_t localSessionPeerId, NetMatchConfig& roster, std::string* error);
		friend bool TestFinishMatchDrainsFencedDisconnect(std::string* error);
		friend bool TestGnsStopCancelContracts(std::string* error);
		friend bool TestEndedWorldLateAdmission(std::string* error);
		friend bool TestServiceDirectoryIceLeaseKeepsIdentity(std::string* error);
		friend bool TestServiceIceRematchPlaysTwoRounds(std::string* error);
		friend bool TestCompletedLobbyIsNotARecovery(std::string* error);
		friend bool TestCompletedLobbyExpires(std::string* error);
		friend bool TestChatSendRefusedOutsideCarry(std::string* error);
		/// Points the coordinator's handover at the service queue the pump drains. Caller holds the lock
		/// only where the match is already launched.
		void AttachCoordinatorSessionSink();
		/// Delivers the handover queue through the session before a teardown destroys the coordinator
		/// that filled it. Caller holds the lock. The census may only open where the sim stands at a
		/// completed tick with the world still up.
		void DrainPendingSessionEventsLocked(bool atTickBoundary);
		/// Keeps next-lobby packets until the rematch worker takes the link.
		void QueueLobbyEvent(const NetTransportEvent& event);
		/// Polls a finished session without touching the ended simulation; caller holds the lock.
		void PumpCompletedSessionLocked();
		/// Refuses Ready peers absent from the ended round; caller holds the lock.
		void RefuseEndedPeersLocked(const std::string& reason);
		/// The relaunch's queue reset, with a permanent diagnostic for anything a teardown left behind.
		void DiscardUndeliveredSessionEventsLocked();
		/// Folds the coordinator's counters into the service so a gate can read them across a resync.
		void AccumulateLockstepTotalsLocked();
		/// Client: the §7 leave protocol, waiting exactly P21's budget for the ack before giving up and
		/// KEEPING the ticket. Runs only with a plane attached and a record to lose.
		void RunCleanLeave();
		/// The worker half of a leave: the §7 exchange, then - and only then - the round is told.
		void LeaveWorkerMain(std::string result);
		/// Ends the hosted session: tells every peer with the one reason that permits deleting a
		/// recovery record (P22), then clears the registry, the ledger and the seats. Caller holds the lock.
		void EndAdmissionSession();
		void ResetRosterTransitionHistory();
		void RecordRosterTransitions(uint64_t observedAtMs);
		/// Publishes a successful local host action to the presentation sink; caller holds the lock.
		void RecordModerationAction(uint16_t stableSeat, NetModerationAction action);
		/// Runs the §11 automatic-retry schedule from the service's own state. Game thread only.
		void DriveReconnectUx(uint64_t nowMs);
		/// Destroys a rematch lobby whose peers did not all come back inside c_CompletedLobbyExpiryMs.
		/// Game thread only, from Update(): it takes the lock and then destroys without it.
		void UpdateCompletedLobbyExpiry(uint64_t nowMs);
		/// Whether every non-CPU seat of the current lobby is connected. Caller holds the lock.
		bool RematchLobbySeatedLocked() const;
		/// Elapsed milliseconds since this session began, for every admission deadline.
		uint64_t AdmissionNowMs() const;
		void CaptureA7SeatView();
		void CacheDiagnosticIdentity(const NetIdentityManifest& manifest);
		bool WaitForA7ConnectGate(std::string* error);
		/// Captures the round before its coordinator or activity is torn down. Caller holds the lock.
		void CaptureMatchSummaryLocked(const std::string& result);
		/// Counts public seat transitions independently of the bounded diagnostic history.
		void UpdateSummarySeatsLocked();


		mutable std::mutex m_Mutex;
		std::string m_DiagnosticIdentity;
		std::string m_DiagnosticRuntimeError;
		static uint32_t s_AutosaveSeconds;
		static bool s_AutosaveSecondsOverridden;
		std::string m_AutosaveMatchId;
		uint32_t m_MatchAutosaveSeconds = 0; //!< The cadence the round agreed on, read once so the tick path never chases the runner.
		int64_t m_NextAutosaveSimTime = -1;
		int64_t m_LastAutosaveSimTime = -1;
		NetMatchServiceState m_State = NetMatchServiceState::Idle;
		std::string m_StatusText = "Idle";
		std::string m_ErrorText;
		std::string m_ActivityPreset;
		std::string m_ActivityModule;
		std::thread m_Worker;
		bool m_WorkerDone = false;
		bool m_IsHost = false;
		uint8_t m_LocalPeerId = 0;
		int m_LocalTeam = -1;
		bool m_Dedicated = false;
		int m_HumanSeats = 0;
		NetMatchConfig m_MatchConfig; //!< The roster this peer asked for, until the round adopts the host's.
		bool m_ResyncOnDesync = false;
		std::string m_PendingResyncLoad;
		std::optional<NetResyncState> m_PendingResyncState;
		bool m_ResyncRetainsLocalState = false;
		uint64_t m_ResyncSourceRound = 0;
		//!< The last host snapshot's tick label and the completed tick it was taken at; a gate asserts they match.
		std::atomic<uint64_t> m_ResyncSavedTick{UINT64_MAX};
		std::atomic<uint64_t> m_ResyncBoundaryTick{UINT64_MAX};
		std::string m_LocalName;
		NetLobbySnapshot m_LobbySnapshot;
		std::optional<NetMatchSummary> m_LastMatchSummary;
		NetMatchSummary m_CurrentMatchSummary;
		std::map<uint8_t, NetSeatPresenceEntry> m_SummarySeats;
		NetSeatAuthRegistry m_SeatAuth; //!< Hosted-session reconnect-auth material (off-sim epoch + seat credentials); survives resync/rejoin/rematch.
		// The admission plane lives on the service, not on a session or a match round, so a seat and its
		// ledger survive resync, rejoin and rematch exactly as the registry does (§3).
		NetReconnectHost m_ReconnectHost;
		NetReconnectClient m_ReconnectClient;
		NetReconnectTicketStore m_TicketStore;
		NetReconnectUx m_ReconnectUx;
		NetSeatPresence m_SeatPresence;
		struct RosterTransition {
			uint8_t peerId = 0;
			std::string state;
			std::string line;
			uint64_t appliedFrame = 0;
			uint64_t observedAtMs = 0;
		};
		std::vector<RosterTransition> m_RosterTransitions;
		uint32_t m_RosterTransitionsDropped = 0;
		std::map<uint8_t, std::pair<std::string, std::string>> m_LastRosterPair;
		std::vector<NetH4ModerationSeat> m_ModerationSeats; //!< Immutable UI copy while a setup/resync worker owns the plane.
		bool m_AdmissionAttached = false;
		bool m_LeaveExchangeRun = false; //!< The §7 exchange has been attempted for this session; Destroy must not repeat it.
		bool m_MatchWasRunning = false;  //!< This session reached a running match, so §11's recovery applies to losing it.
		uint64_t m_LastUpdateMs = 0;     //!< The millisecond Update() last ran, so two callers in one frame do one pump.
		std::vector<NetH4SeatStatus> m_SeatStatuses; //!< Published from the sim pump for the roster (§11).
		std::string m_InputDelayText; //!< The announced input-delay line, built beside each lobby publish.
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
		static std::string s_JoinWaitPath;
		static bool s_ApplyForSeat;
		static uint16_t s_ApplySeat;
		static bool s_ApplyOnce; //!< The menu's one-shot application; consumed by the next join's plane.
		bool m_JoinRefusedByLiveMatch = false; //!< The last join was refused by a running match (§9b).
		static bool s_AutoSubstitute;
		static uint16_t s_AutoSubstituteSeat;
		static uint64_t s_AutoSubstituteDelayMs;
		static bool s_AutoSubstituteThenCancel;
		uint64_t m_AutoSubstituteReadyMs = 0; //!< When the stand-in first saw an applicant it could approve.
		bool m_AutoSubstituteDone = false;

		std::unique_ptr<GnsTransport> m_Transport;
		//!< ICE runs only; the direct-IP path keeps the plain transport above untouched.
		std::unique_ptr<NetMuxTransport> m_Mux;
#ifdef CCCP_WITH_GNS
		//!< The session directory's signal relay; pumped by the mux on the transport-owner thread.
		std::unique_ptr<GnsDirectorySignalDispatcher> m_Dispatcher;
#endif
		bool m_IceEnabled = false;          //!< This run offers (host) or takes (client) a session-id join.
		std::string m_IceBoundSessionId;    //!< The session id the process's GNS identity is pinned to.
		std::string m_IceIdentity;
		std::string m_IceJoinSessionId;     //!< Client: the session id -net-join-session named.
		std::string m_IceReport;            //!< The dispatcher's last report, taken when a worker or teardown takes the dispatcher.
		std::string m_IceRoute;             //!< The leg the join actually took: "ice" | "ip" | "".
		//!< Published by Update() for the worker: the directory client is game-thread only.
		std::string m_DirectorySessionId;
		std::string m_DirectoryToken;
		bool m_DirectoryRegistered = false;
		std::unique_ptr<NetSession> m_Session;
		std::unique_ptr<NetLockstepCoordinator> m_Coordinator;
		std::unique_ptr<NetMatchRunner> m_Runner;
		std::vector<NetTransportEvent> m_PendingLobbyEvents;
		size_t m_PendingLobbyBytes = 0;
		bool m_PendingLobbyOverflow = false;
		bool m_LeftMatch = false;
		//!< Steady ms of the match end that opened this rematch lobby; 0 when no lobby is waiting.
		uint64_t m_CompletedLobbySinceMs = 0;
		uint64_t m_EndedLockstepPackets = 0;
		// Non-owning view of the live session object: while the runner's worker still owns it
		// (the whole lobby phase) m_Session is empty, but chat must already reach it.
		NetSession* m_ChatSession = nullptr;
		std::vector<NetTransportEvent> m_PendingSessionEvents; //!< Game-thread only: reconnect traffic the coordinator handed over.
		//!< Coordinator counters a resync would otherwise zero, accumulated at every teardown.
		struct LockstepTotals {
			uint64_t peerFramesWaived = 0;
			uint64_t peersDroppedSilent = 0;
			uint64_t connectionsClosedOnEviction = 0;
		};
		LockstepTotals m_LockstepTotals;
		uint32_t m_SessionEventsDrained = 0;   //!< Handover events delivered by a teardown instead of the pump.
		uint32_t m_SessionEventsDiscarded = 0; //!< Handover events a relaunch found undelivered; must stay zero.
		NetAdmissionClock m_AdmissionClock; //!< One elapsed-time source for setup, play, stalls and resync.
		NetLanDiscovery m_LanDiscovery; //!< Game-thread only: the hosting lobby's LAN beacon.
		/// Game-thread only, like the beacon: the host's session-directory row. Unlike the beacon it
		/// stays listed while the match runs so a late joiner can still resolve it.
		NetDirectoryClient m_Directory;
		NetDirectoryRegisterRequest m_DirectoryRow; //!< The listing template; counts refresh per Update.
		bool m_DirectoryRetracted = false;          //!< The match ended while the state was still Running.
		bool m_DirectoryHidden = false;             //!< A natural ICE end keeps the bound row unlisted.
		bool m_DirectoryRelistPending = false;      //!< The next lobby awaits the hide acknowledgement.
		uint16_t m_BeaconGamePort = 0;
		uint8_t m_BeaconMaxPlayers = 2;
		std::atomic<bool> m_ReadyRequested{false};
		std::atomic<bool> m_StartRequested{false};
		std::atomic<bool> m_CancelRequested{false};
		std::atomic<bool> m_EverStarted{false};
		std::string m_CapturedRunnerReport;
		std::string m_RejoinOutcome;
		struct LastResyncMetrics {
			uint64_t archiveBytes = 0;
			uint64_t envelopeBytes = 0;
			uint64_t saveMs = 0;
			uint64_t transferMs = 0;
			uint64_t healMs = 0;
			bool happened = false;
		};
		LastResyncMetrics m_LastResync;
		uint64_t m_ResyncHealStartMs = 0;
		bool m_ResyncHealOpen = false;
		bool m_HostLobbyBeaconed = false;
	};

} // namespace RTE
