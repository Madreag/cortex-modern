#pragma once

#include "NetLobbySession.h"
#include "NetLobbySnapshot.h"
#include "NetLockstep.h"
#include "NetSession.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
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

	struct NetMatchRunnerConfig {
		bool host = false;
		std::string joinAddress;
		NetSessionConfig sessionConfig;
		NetMatchConfig matchConfig;
		bool autoInputDelay = false; // Host: raise matchConfig.inputDelayFrames to cover the measured RTT.
		bool useLobbyProtocol = false;
		uint64_t startFrame = 0;
		uint32_t sessionWaitMs = 15000;
		uint32_t lobbyWaitMs = 15000;
		uint32_t lockstepWaitMs = 5000;
		// In-match missing-frame grace before the match is declared dead; the setup wait above stays short.
		uint32_t missingFrameGraceMs = 20000;
		uint32_t postSessionSettleMs = 250;
		uint32_t postLobbySettleMs = 250;
		std::string scenario;
		bool autoReady = true;
		bool autoStart = true;
		const std::atomic<bool>* readyRequested = nullptr;
		std::atomic<bool>* startRequested = nullptr;
		const std::atomic<bool>* cancelRequested = nullptr;
		std::function<void(const NetLobbySnapshot&)> publishLobby;
		// The session's clock. Supplied by the service so setup, play and every resync share one elapsed
		// time; without it each wait clocks from its own start, which the admission deadlines cannot use.
		std::function<uint64_t()> nowMs;
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

		bool Start(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, const NetMatchRunnerConfig& config, std::string* error = nullptr);

		/// Runs the next match over an already-established session: re-runs the lobby round and starts a
		/// fresh coordinator, reusing the config from Start(). The prior match must have ended cleanly.
		/// A host may hand in a match-state file to stream to every peer during the round (a resync).
		bool StartNextMatch(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, std::string* error = nullptr, std::vector<uint8_t> stateToStream = {});

		/// Takes the state file the lobby round received (empty when the round carried none).
		std::vector<uint8_t> TakeReceivedState() { return std::move(m_ReceivedStateBytes); }

		/// Sets the first lockstep tick of the next round; the lobby start carries it to the clients.
		void SetStartFrame(uint64_t startFrame) { m_Config.startFrame = startFrame; }

		/// Client: the peers ITS round still had when it ended. The next rematch derives this peer's own
		/// roster from them and refuses a host proposal that does not fit it. A host derives its roster
		/// from the live session instead, so it needs no list; a resync round keeps the roster it healed.
		void SetRematchRoster(std::vector<uint8_t> survivingPeerIds) { m_RematchRoster = std::move(survivingPeerIds); }

		NetMatchRuntimeState GetState() const { return m_State; }
		const NetLobbySession& GetLobbySession() const { return m_Lobby; }
		const NetMatchConfig& GetMatchConfig() const { return m_MatchConfig; }
		const NetHash32& GetMatchConfigHash() const { return m_MatchConfigHash; }
		bool UsesLobbyProtocol() const { return m_UseLobbyProtocol; }
		const std::string& GetSetupError() const { return m_SetupError; }

		std::string BuildReportJson(const NetSession& session, const NetLockstepCoordinator& coordinator) const;

		/// Client: the survivors to hand SetRematchRoster, from what its own round saw.
		static std::vector<uint8_t> DeriveRematchSurvivors(const NetMatchConfig& played, const std::map<uint8_t, uint64_t>& leaveFrames, const std::set<uint8_t>& refilledPeerIds, const NetLockstepSeatSnapshot* seats);
		/// Whether a host's rematch proposal is the roster this peer derived, less seats only the host knows are gone.
		static bool RematchRostersAgree(const NetMatchConfig& proposed, const NetMatchConfig& derived, uint8_t localPeerId, std::string* reason = nullptr);

		static const char* StateName(NetMatchRuntimeState state);

	private:
		/// Re-forms the roster the next round is played on and re-seats everything that depends on it.
		bool PrepareRematchRoster(NetSession& session, const std::vector<uint8_t>& survivingPeerIds, std::string* error);
		/// Client: the host's proposal must fit the roster this peer derived, on the seat it was admitted on.
		bool VerifyRematchProposal(uint8_t localPeerId, std::string* error);
		bool WaitForSessionReady(INetTransport& transport, NetSession& session, uint32_t expectedReadyPeers, uint64_t maxWaitMs, std::string* error);
		bool RunLobby(INetTransport& transport, NetSession& session, uint64_t maxWaitMs, std::string* error);
		bool StartLockstep(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, const NetMatchRunnerConfig& config, std::string* error);
		bool WaitForLockstepRunning(NetLockstepCoordinator& coordinator, uint64_t maxWaitMs, std::string* error);
		NetLobbySnapshot BuildLobbySnapshot(const INetTransport& transport, const NetSession& session) const;
		// Lockstep peer ids are 1-based and dense; the session assigns the host id 0 and clients 1.. .
		std::map<uint8_t, NetPeerId> BuildRemoteTransportMap(const NetSession& session) const;
		uint8_t LocalLockstepPeerId(const NetSession& session) const;
		void SetFailed(const std::string& error);

		NetMatchRuntimeState m_State = NetMatchRuntimeState::Idle;
		NetMatchRunnerConfig m_Config;
		NetLobbySession m_Lobby;
		NetMatchConfig m_MatchConfig;
		NetHash32 m_MatchConfigHash{};
		bool m_UseLobbyProtocol = false;
		bool m_ResyncRound = false;
		std::vector<uint8_t> m_RematchRoster; //!< Client: the peers its last round still had; consumed by the next rematch.
		NetMatchConfig m_RematchConfig;       //!< This peer's own derivation of the rematch roster.
		bool m_RematchRound = false;
		std::string m_SetupError;
		std::vector<uint8_t> m_StateToStream; //!< Host: a match-state file the next lobby round streams out.
		std::vector<uint8_t> m_ReceivedStateBytes; //!< The state file the last lobby round received.
	};

} // namespace RTE
