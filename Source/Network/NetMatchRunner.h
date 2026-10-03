#pragma once

#include "NetLobbySession.h"
#include "NetLobbySnapshot.h"
#include "NetLockstep.h"
#include "NetSession.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace RTE {

	enum class NetMatchRuntimeState {
		Idle,
		SessionStarting,
		LobbySync,
		LockstepStarting,
		Running,
		Failed,
	};

	/// The one host-options draft waiting for the runner thread, posted by the service's Apply. Newest
	/// wins: a second Apply before the round takes the first replaces it, which is what the host meant.
	/// The flag is the cheap poll the lobby loop reads every tick; the mutex only guards the config
	/// itself, so the runner never blocks the game thread for the length of a copy.
	struct NetHostOptionsSlot {
		std::atomic<bool> pending{false};
		std::mutex mutex;
		NetMatchConfig config;
		std::string refusal;

		/// Service thread: stages the accepted draft for the runner.
		void Post(const NetMatchConfig& draft) {
			std::lock_guard<std::mutex> lock(mutex);
			config = draft;
			refusal.clear();
			pending.store(true, std::memory_order_release);
		}
		/// Runner thread: takes the staged draft, if one is waiting.
		bool Take(NetMatchConfig& out) {
			if (!pending.load(std::memory_order_acquire)) {
				return false;
			}
			std::lock_guard<std::mutex> lock(mutex);
			return TakeLocked(out);
		}
		bool TakeLocked(NetMatchConfig& out) {
			// Clear can win after the runner's cheap poll and before its lock.
			if (!pending.load(std::memory_order_acquire)) return false;
			out = config;
			pending.store(false, std::memory_order_release);
			return true;
		}
		/// Drops a draft whose session is gone, so the next one never inherits it.
		void Clear() {
			std::lock_guard<std::mutex> lock(mutex);
			config = {};
			refusal.clear();
			pending.store(false, std::memory_order_release);
		}
	};

	struct NetMatchRunnerConfig {
		bool host = false;
		std::string joinAddress;
		std::function<std::string()> resolveJoinAddress;
		NetSessionConfig sessionConfig;
		NetMatchConfig matchConfig;
		bool autoInputDelay = false; // Host: raise matchConfig.inputDelayFrames to cover the measured RTT.
		std::function<bool(NetRelayConfig&)> relayOffer;
		bool useLobbyProtocol = false;
		uint64_t startFrame = 0;
		uint32_t sessionWaitMs = 15000;
		// The technical deadline a setup round waits to HEAR from its peers. It is fixed by the
		// protocol, never by a host's policy, and it is what a client's round patience reads.
		uint32_t lobbyWaitMs = 15000;
		// Host: how long an unseated lobby stays open waiting for people, which is the host's own idle
		// policy. 0 means Never and the round only ends when the host or a peer ends it; unset keeps
		// the message deadline as the budget, which is what a round without a seating policy had.
		std::optional<uint32_t> lobbySeatingWaitMs;
		uint32_t lockstepWaitMs = 5000;
		// A world joiner's start runs inside its sim update, so its deadline counts that peer's own
		// updates: a wall clock read inside the tick is a per-machine decision.
		uint32_t worldJoinStartWaitTicks = 600;
		// In-match missing-frame grace before the match is declared dead; the setup wait above stays short.
		uint32_t missingFrameGraceMs = 20000;
		uint32_t postSessionSettleMs = 250;
		uint32_t postLobbySettleMs = 250;
		std::string scenario;
		bool autoReady = true;
		bool autoStart = true;
		const std::atomic<bool>* readyRequested = nullptr;
		std::atomic<bool>* startRequested = nullptr;
		// Host: the round's start scripts, streamed ahead of the lobby start the first time a start is asked for.
		std::function<std::vector<uint8_t>()> roundStartScripts;
		const std::atomic<bool>* cancelRequested = nullptr;
		// Host: accepted host-options drafts on their way to this thread. The lobby loop republishes
		// one as the round's next configuration revision; a rematch starts its round on it.
		NetHostOptionsSlot* hostOptions = nullptr;
		std::function<void(const NetLobbySnapshot&)> publishLobby;
		// The session's clock. Supplied by the service so setup, play and every resync share one elapsed
		// time; without it each wait clocks from its own start, which the admission deadlines cannot use.
		std::function<uint64_t()> nowMs;
		bool enableMigration = false;
		std::vector<std::string> migrationListenAddrs;
		std::function<bool(uint8_t, const NetHash32&, std::vector<uint8_t>&)> sealMigration;
		std::function<bool(const NetLobbyMigration&)> openMigration;
		std::function<void(NetLockstepConfig&)> configureMigration;
		// Host: the checkpoint a match resumed from disk stands on. The lobby offers it to every peer
		// and streams its state only to those that do not already hold that archive.
		std::string resumeMatchId;
		uint64_t resumeTick = 0;
		std::string resumeDigest;
		std::string resumeSideStateHash;
		std::function<bool(const NetLobbyResume&)> resumeHeld;
		/// Host: apply a Starting-state kick on this worker after the session tick, never from the game thread.
		/// The runner hands back the session it just ticked, which the worker owns for the whole setup.
		std::function<void(NetSession&)> pumpHost;
	};

	/// What a setup round clocks each of its parts with.
	struct NetMatchRunnerClocks {
		uint64_t lobbyMs = 0;  //!< Lobby retransmissions and its own wait budget use time since round start.
		uint64_t planeMs = 0;  //!< The admission plane's deadlines are session-elapsed time.
		uint64_t budgetMs = 0; //!< The round's own wait budget, which is per round like the lobby's.
	};

	class NetMatchRunner {
	public:
		/// Keeps lobby wait intervals separate from admission's session-elapsed deadlines.
		static NetMatchRunnerClocks ResolveRoundClocks(uint64_t roundMs, bool hasSessionClock, uint64_t sessionClockMs) {
			return {roundMs, hasSessionClock ? sessionClockMs : roundMs, roundMs};
		}

		/// Whether a setup round has waited out its seating time. The host's Never (a seating wait of
		/// 0) never does, however long the lobby stays open; a round with no seating policy of its own
		/// budgets by its message deadline, which is what every round did before the two were split.
		static bool SeatingWaitExpired(const std::optional<uint32_t>& seatingWaitMs, uint32_t messageDeadlineMs, uint64_t sinceProgressMs) {
			if (seatingWaitMs && *seatingWaitMs == 0) {
				return false;
			}
			return sinceProgressMs > seatingWaitMs.value_or(messageDeadlineMs);
		}

		bool Start(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, const NetMatchRunnerConfig& config, std::string* error = nullptr);

		/// Runs the next match over an already-established session: re-runs the lobby round and starts a
		/// fresh coordinator, reusing the config from Start(). The prior match must have ended cleanly.
		/// A host may hand in a match-state file to stream to every peer during the round (a resync).
		bool StartNextMatch(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, std::string* error = nullptr, std::vector<uint8_t> stateToStream = {}, std::vector<NetTransportEvent> pendingLobbyEvents = {});

		/// Takes the state file the lobby round received (empty when the round carried none).
		std::vector<uint8_t> TakeReceivedState() { return std::move(m_ReceivedStateBytes); }

		/// Host: the match state the NEXT lobby round streams out, for a round Start() opens rather than
		/// StartNextMatch(). The bytes stay out of the config so the round never copies a whole archive.
		void SetStateToStream(std::vector<uint8_t> bytes) { m_StateToStream = std::move(bytes); }

		/// Sets the first lockstep tick of the next round; the lobby start carries it to the clients.
		void SetStartFrame(uint64_t startFrame) { m_Config.startFrame = startFrame; }
		void AdoptHostMigration(const NetHostMigrationResult& result, uint8_t localPeerId);
		uint8_t GetSnapshotProviderPeerId() const { return m_SnapshotProviderPeerId; }
		bool DidLoseHostDuringSetup() const { return m_HostLostDuringSetup; }

		/// Client: its round ended into a rematch lobby, so the next round is that rematch and the host's proposal must keep
		/// every seat; a resync round keeps the roster it healed.
		void SetRematchOwed(bool owed) { m_RematchOwed = owed; }

		NetMatchRuntimeState GetState() const { return m_State; }
		NetLobbySession& GetLobbySession() { return m_Lobby; }
		const NetLobbySession& GetLobbySession() const { return m_Lobby; }
		bool TookWorldJoinImage() const { return m_WorldJoinImage; }
		/// Starts the joiner's lockstep at its activation tick without blocking the sim update it runs
		/// inside: the handshake finishes over the pumps that follow.
		/// @return Whether the coordinator is already running.
		void ConfigurePrivateJoin(const NetLockstepConfig& config) { m_PrivateJoinConfig = config; m_MatchConfig = config.matchConfig; m_ActiveHostPeerId = config.authorityPeerId; }
		bool StartWorldJoinLockstep(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, uint64_t startFrame, uint64_t updateTick, std::string* error = nullptr);
		/// One tick of a starting joiner's handshake. Returns whether the coordinator is running; a
		/// false with an error set is the start giving up.
		bool PumpWorldJoinLockstepStart(NetLockstepCoordinator& coordinator, uint64_t updateTick, std::string* error = nullptr);
		/// Drops a joiner's lockstep start whose activation the host has taken back; the round it follows goes on.
		void CancelWorldJoinLockstepStart() { if (m_WorldJoinStarting) { m_WorldJoinStarting = false; m_State = NetMatchRuntimeState::Running; } }
		/// Whether a joiner's lockstep start is mid-handshake and wants its tick this pump.
		bool IsWorldJoinLockstepStarting() const { return m_WorldJoinStarting && m_State == NetMatchRuntimeState::LockstepStarting; }
		/// How many of the joiner's own updates the start has cost so far.
		uint32_t GetWorldJoinStartTicks() const { return m_WorldJoinStartTicks; }
		const NetMatchConfig& GetMatchConfig() const { return m_MatchConfig; }
		/// The seat roster revision this peer's round started on, checked against its own copy; 0 for a host or before a checked start.
		uint32_t GetRosterAgreedRevision() const { return m_RosterAgreedRevision; }
		void SetRelayOffer(const NetRelayConfig& offer) {
			m_MatchConfig.relay = offer;
			m_Config.matchConfig.relay = offer;
			m_Lobby.SetRelayOffer(offer);
		}
		const NetHash32& GetMatchConfigHash() const { return m_MatchConfigHash; }
		bool UsesLobbyProtocol() const { return m_UseLobbyProtocol; }
		/// Whether this round loads a checkpoint, however the checkpoint reached this peer: streamed by
		/// the host, or already held here and therefore never streamed.
		static bool RoundResumesASnapshot(bool resyncRound, bool receivedState, bool answeredResumeHeld) {
			return resyncRound || receivedState || answeredResumeHeld;
		}
		const std::string& GetSetupError() const { return m_SetupError; }
		bool HasRefusedHostOptions() const { return m_HostOptionsRefused; }

		std::string BuildReportJson(const NetSession& session, const NetLockstepCoordinator& coordinator) const;

		/// Host: a round's members, ascending: the host and each named peer of the round's seats. Every seat is kept; one not named
		/// starts the round held, its player returning through the rejoin.
		/// @param presentPeerIds The peers the round's start waits on: the seat roster's, or with no admission plane the ready links.
		static std::vector<uint8_t> RematchMembers(uint8_t hostPeerId, uint8_t peerCount, const std::vector<uint8_t>& presentPeerIds);
		/// A round's active members once its lobby has started: a lobby round's are the agreed config's on the host and every client
		/// alike (none named means every seat), so no peer starts on a member set another does not hash.
		/// @param lobbyAgreed Whether a lobby agreed the round's config.
		/// @param formed The members the round was formed with.
		/// @param agreed The config the lobby agreed.
		/// @return The members the round's start waits for.
		static std::vector<uint8_t> SettledRoundMembers(bool lobbyAgreed, const std::vector<uint8_t>& formed, const NetMatchConfig& agreed);
		/// The setup error of a client whose seat the agreed round starts held: it takes the round through the held rejoin, not a start.
		static constexpr const char* c_SeatStartsHeld = "this seat starts the round held";
		/// From a round's formation its coordinator owns the wire: the session's traffic on it is queued here, in order and bounded,
		/// until a reader takes it - the setup worker while it waits for the round, the service once it takes the round over.
		void CarrySessionTraffic(NetLockstepCoordinator& coordinator);
		/// Hands the queued session traffic to the session, oldest first.
		void DeliverSessionTraffic(NetSession& session, uint64_t nowMs);
		/// The queued session traffic, oldest first, for the reader that takes the round over.
		std::vector<NetTransportEvent> TakeSessionTraffic();
		/// Whether a host's rematch proposal keeps every seat of the round this peer derived with its id, team and kind.
		static bool RematchKeepsEverySeat(const NetMatchConfig& proposed, const NetMatchConfig& derived, std::string* reason = nullptr);

		static const char* StateName(NetMatchRuntimeState state);

	private:
		// A refused draft stays available so the host can see why its rematch did not start.
		bool AdoptStagedHostOptions(std::string* error);
		/// Host: takes the seating wait from the published idle policy, so a live edit of it lands.
		void SyncSeatingWaitToConfig();
		/// Forms the config the next round is played on: every seat kept, the host naming who is present.
		bool PrepareRematchRoster(NetSession& session, std::string* error);
		/// Client: the host's proposal must keep every seat of the round this peer played.
		bool VerifyRematchProposal(std::string* error);
		bool WaitForSessionReady(INetTransport& transport, NetSession& session, uint32_t expectedReadyPeers, uint64_t maxWaitMs, std::string* error);
		/// A round starts only on the seat roster the host agreed it on: a peer that heard another is refused by name.
		bool AgreeOnSeatRoster(NetSession& session, std::string* error);
		uint32_t m_RosterAgreedRevision = 0;
		static constexpr uint64_t c_RosterRevisionWaitMs = 2000; //!< How long a start waits for the revision it asked the host for.
		bool RunLobby(INetTransport& transport, NetSession& session, uint64_t maxWaitMs, std::string* error, std::vector<NetTransportEvent> pendingEvents = {});
		bool StartLockstep(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, const NetMatchRunnerConfig& config, std::string* error);
		bool WaitForLockstepRunning(NetLockstepCoordinator& coordinator, uint64_t maxWaitMs, std::string* error, NetSession* session = nullptr);
		NetLobbySnapshot BuildLobbySnapshot(const INetTransport& transport, const NetSession& session) const;
		friend bool TestKickedSeatReadsOpen(std::string* error);
		// Lockstep peer ids are 1-based and dense; the session assigns the host id 0 and clients 1.. .
		std::map<uint8_t, NetPeerId> BuildRemoteTransportMap(const NetSession& session) const;
		uint8_t LocalLockstepPeerId(const NetSession& session) const;
		void SetFailed(const std::string& error);

		NetMatchRuntimeState m_State = NetMatchRuntimeState::Idle;
		NetMatchRunnerConfig m_Config;
		NetLobbySession m_Lobby;
		NetMatchConfig m_MatchConfig;
		NetHash32 m_MatchConfigHash{};
		std::optional<NetLockstepConfig> m_PrivateJoinConfig;
		bool m_UseLobbyProtocol = false;
		bool m_ResyncRound = false;
		bool m_HostLostDuringSetup = false;
		bool m_HostOptionsRefused = false;
		bool m_RematchOwed = false; //!< Client: its last round ended into a rematch lobby; consumed by the next round.
		std::string m_LastRosterStampRefusal; //!< Host: a refused roster republish, named once.
		std::deque<NetTransportEvent> m_SessionTraffic; //!< Session traffic the coordinator owned the wire for, waiting for a reader.
		uint32_t m_SessionTrafficDropped = 0; //!< Events past the queue's bound, named once.
		NetMatchConfig m_RematchConfig;       //!< This peer's own derivation of the rematch roster.
		bool m_RematchRound = false;
		uint8_t m_ActiveHostPeerId = 0;
		uint8_t m_SnapshotProviderPeerId = 0;
		std::vector<uint8_t> m_ActivePeerIds;
		std::string m_SetupError;
		std::vector<uint8_t> m_StateToStream; //!< Host: a match-state file the next lobby round streams out.
		std::vector<uint8_t> m_ReceivedStateBytes; //!< The state file the last lobby round received.
		bool m_WorldJoinImage = false;
		bool m_WorldJoinStarting = false;      //!< A joiner's lockstep start is mid-handshake.
		std::optional<uint64_t> m_WorldJoinStartLastTick;
		uint32_t m_WorldJoinStartTicks = 0;    //!< The joiner's own updates that start has cost.
	};

} // namespace RTE
