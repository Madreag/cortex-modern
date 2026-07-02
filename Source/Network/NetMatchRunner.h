#pragma once

#include "NetLobbySession.h"
#include "NetLobbySnapshot.h"
#include "NetLockstep.h"
#include "NetSession.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

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
		bool StartNextMatch(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, std::string* error = nullptr);

		NetMatchRuntimeState GetState() const { return m_State; }
		const NetLobbySession& GetLobbySession() const { return m_Lobby; }
		const NetMatchConfig& GetMatchConfig() const { return m_MatchConfig; }
		const NetHash32& GetMatchConfigHash() const { return m_MatchConfigHash; }
		bool UsesLobbyProtocol() const { return m_UseLobbyProtocol; }
		const std::string& GetSetupError() const { return m_SetupError; }

		std::string BuildReportJson(const NetSession& session, const NetLockstepCoordinator& coordinator) const;

		static const char* StateName(NetMatchRuntimeState state);

	private:
		bool WaitForSessionReady(NetSession& session, uint64_t maxWaitMs, std::string* error);
		bool RunLobby(INetTransport& transport, const NetSession& session, uint64_t maxWaitMs, std::string* error);
		bool StartLockstep(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, const NetMatchRunnerConfig& config, std::string* error);
		bool WaitForLockstepRunning(NetLockstepCoordinator& coordinator, uint64_t maxWaitMs, std::string* error);
		NetLobbySnapshot BuildLobbySnapshot(const INetTransport& transport, const NetSession& session) const;
		void SetFailed(const std::string& error);

		NetMatchRuntimeState m_State = NetMatchRuntimeState::Idle;
		NetMatchRunnerConfig m_Config;
		NetLobbySession m_Lobby;
		NetMatchConfig m_MatchConfig;
		NetHash32 m_MatchConfigHash{};
		bool m_UseLobbyProtocol = false;
		std::string m_SetupError;
	};

} // namespace RTE
