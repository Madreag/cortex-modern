#pragma once

#include "NetLobbySession.h"
#include "NetLobbySnapshot.h"
#include "NetLockstep.h"
#include "NetSession.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
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
		const std::atomic<bool>* startRequested = nullptr;
		const std::atomic<bool>* cancelRequested = nullptr;
		std::function<void(const NetLobbySnapshot&)> publishLobby;
	};

	class NetMatchRunner {
	public:
		bool Start(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, const NetMatchRunnerConfig& config, std::string* error = nullptr);

		/// Runs the next match over an already-established session: re-runs the lobby round and starts a
		/// fresh coordinator, reusing the config from Start(). The prior match must have ended cleanly.
		/// A host may hand in a match-state file to stream to every peer during the round (a resync).
		bool StartNextMatch(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, std::string* error = nullptr, std::vector<uint8_t> stateToStream = {});

		/// Takes the state file the lobby round received (empty when the round carried none).
		std::vector<uint8_t> TakeReceivedState() { return std::move(m_ReceivedStateBytes); }

		NetMatchRuntimeState GetState() const { return m_State; }
		const NetLobbySession& GetLobbySession() const { return m_Lobby; }
		const NetMatchConfig& GetMatchConfig() const { return m_MatchConfig; }
		const NetHash32& GetMatchConfigHash() const { return m_MatchConfigHash; }
		bool UsesLobbyProtocol() const { return m_UseLobbyProtocol; }
		const std::string& GetSetupError() const { return m_SetupError; }

		std::string BuildReportJson(const NetSession& session, const NetLockstepCoordinator& coordinator) const;

		static const char* StateName(NetMatchRuntimeState state);

	private:
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
		std::string m_SetupError;
		std::chrono::steady_clock::time_point m_RunStartTime; //!< One continuous clock across the setup phases for the session keepalive.
		std::vector<uint8_t> m_StateToStream; //!< Host: a match-state file the next lobby round streams out.
		std::vector<uint8_t> m_ReceivedStateBytes; //!< The state file the last lobby round received.
	};

} // namespace RTE
