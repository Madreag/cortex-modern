#include "NetMatchRunner.h"

#include "NetIdentity.h"

#include "nlohmann/json.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace RTE {

	namespace {
		using json = nlohmann::json;

		std::string HashText(const NetHash32& hash) {
			return NetIdentity::HashHex(hash);
		}

		uint8_t RemotePeerFor(bool host) {
			return host ? 2 : 1;
		}

		uint8_t LocalPeerFor(bool host) {
			return host ? 1 : 2;
		}
	}

	bool NetMatchRunner::Start(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, const NetMatchRunnerConfig& config, std::string* error) {
		m_Config = config;
		m_UseLobbyProtocol = config.useLobbyProtocol;
		m_MatchConfig = config.matchConfig;
		m_MatchConfigHash = NetMatchConfigUtil::HashConfig(m_MatchConfig);
		m_SetupError.clear();
		m_State = NetMatchRuntimeState::SessionStarting;

		if (config.host == !config.joinAddress.empty()) {
			SetFailed("match runner requires exactly one host or join address");
			if (error) *error = m_SetupError;
			return false;
		}
		std::string validateError;
		if (!NetMatchConfigUtil::ValidateLocalAlpha(config.matchConfig, &validateError)) {
			SetFailed(validateError);
			if (error) *error = m_SetupError;
			return false;
		}

		NetSessionConfig sessionConfig = config.sessionConfig;
		const bool sessionStarted = config.host
			? session.StartHost(transport, std::move(sessionConfig), error)
			: session.StartClient(transport, config.joinAddress, std::move(sessionConfig), error);
		if (!sessionStarted) {
			SetFailed(error ? *error : "session start failed");
			return false;
		}
		if (!WaitForSessionReady(session, config.sessionWaitMs, error)) {
			return false;
		}
		m_MatchConfig.sessionId = session.GetSessionId();
		m_MatchConfigHash = NetMatchConfigUtil::HashConfig(m_MatchConfig);

		if (config.postSessionSettleMs > 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(config.postSessionSettleMs));
		}

		if (m_UseLobbyProtocol) {
			m_State = NetMatchRuntimeState::LobbySync;
			if (!RunLobby(transport, session, config.lobbyWaitMs, error)) {
				return false;
			}
			if (config.postLobbySettleMs > 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(config.postLobbySettleMs));
			}
		}

		m_State = NetMatchRuntimeState::LockstepStarting;
		if (!StartLockstep(transport, session, coordinator, config, error) || !WaitForLockstepRunning(coordinator, config.lockstepWaitMs, error)) {
			return false;
		}
		m_State = NetMatchRuntimeState::Running;
		return true;
	}

	std::string NetMatchRunner::BuildReportJson(const NetSession& session, const NetLockstepCoordinator& coordinator) const {
		json report{
			{"state", StateName(m_State)},
			{"setup_error", m_SetupError},
			{"uses_lobby_protocol", m_UseLobbyProtocol},
			{"match_config_hash", HashText(m_MatchConfigHash)},
			{"match_config", json::parse(NetMatchConfigUtil::BuildReportJson(m_MatchConfig))},
			{"session", json::parse(session.BuildReportJson())},
			{"lockstep", json::parse(coordinator.BuildReportJson())},
		};
		if (m_UseLobbyProtocol && m_Lobby.GetState() != NetLobbyState::Idle) {
			report["lobby"] = json::parse(m_Lobby.BuildReportJson());
		}
		return report.dump();
	}

	const char* NetMatchRunner::StateName(NetMatchRuntimeState state) {
		switch (state) {
			case NetMatchRuntimeState::Idle: return "Idle";
			case NetMatchRuntimeState::SessionStarting: return "SessionStarting";
			case NetMatchRuntimeState::LobbySync: return "LobbySync";
			case NetMatchRuntimeState::LockstepStarting: return "LockstepStarting";
			case NetMatchRuntimeState::Running: return "Running";
			case NetMatchRuntimeState::Failed: return "Failed";
		}
		return "Unknown";
	}

	bool NetMatchRunner::WaitForSessionReady(NetSession& session, uint64_t maxWaitMs, std::string* error) {
		const auto startTime = std::chrono::steady_clock::now();
		while (true) {
			if (m_Config.cancelRequested && m_Config.cancelRequested->load()) {
				SetFailed("match setup canceled");
				if (error) *error = m_SetupError;
				return false;
			}
			const uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - startTime).count());
			session.Tick(nowMs);
			if (session.IsReady()) {
				return true;
			}
			if (session.IsRejected() || session.IsFailed() || session.IsClosed()) {
				// Surface the recorded mismatch (mod/config/version, timeout) instead of the bare state name.
				const std::string rejectText = session.BuildRejectText();
				SetFailed(!rejectText.empty() ? rejectText : std::string("session did not reach Ready; state=") + NetSession::StateName(session.GetState()));
				if (error) *error = m_SetupError;
				return false;
			}
			if (nowMs > maxWaitMs) {
				SetFailed("timed out waiting for session Ready");
				if (error) *error = m_SetupError;
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}

	bool NetMatchRunner::RunLobby(INetTransport& transport, const NetSession& session, uint64_t maxWaitMs, std::string* error) {
		NetLobbySessionConfig lobbyConfig;
		lobbyConfig.host = m_Config.host;
		lobbyConfig.localPeerId = LocalPeerFor(m_Config.host);
		lobbyConfig.remotePeerId = RemotePeerFor(m_Config.host);
		lobbyConfig.remoteTransportPeerId = session.GetRemoteTransportPeerId();
		lobbyConfig.matchConfig = m_MatchConfig;
		lobbyConfig.startFrame = m_Config.startFrame;
		lobbyConfig.displayName = m_Config.sessionConfig.displayName;
		lobbyConfig.platform = m_Config.sessionConfig.localIdentity.platform;
		lobbyConfig.autoReady = m_Config.autoReady;
		lobbyConfig.autoStart = m_Config.autoStart;
		if (!m_Lobby.Start(transport, lobbyConfig, error)) {
			SetFailed(error ? *error : "lobby start failed");
			return false;
		}

		const auto startTime = std::chrono::steady_clock::now();
		while (true) {
			if (m_Config.cancelRequested && m_Config.cancelRequested->load()) {
				SetFailed("match setup canceled");
				if (error) *error = m_SetupError;
				return false;
			}
			if (m_Config.readyRequested && m_Config.readyRequested->load()) {
				m_Lobby.SetLocalReady(true);
			}
			if (m_Config.startRequested && m_Config.startRequested->load()) {
				m_Lobby.RequestStart();
			}
			const uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - startTime).count());
			m_Lobby.Tick(nowMs);
			if (m_Config.publishLobby) {
				m_Config.publishLobby(BuildLobbySnapshot(transport, session));
			}
			if (m_Lobby.IsStarted()) {
				m_MatchConfig = m_Lobby.GetMatchConfig();
				m_MatchConfigHash = m_Lobby.GetMatchConfigHash();
				return true;
			}
			if (m_Lobby.IsFailed() || m_Lobby.IsRejected()) {
				SetFailed(m_Lobby.GetFailureReason());
				if (error) *error = m_SetupError;
				return false;
			}
			if (nowMs > maxWaitMs) {
				SetFailed("timed out waiting for lobby start");
				if (error) *error = m_SetupError;
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}

	bool NetMatchRunner::StartLockstep(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, const NetMatchRunnerConfig& config, std::string* error) {
		NetLockstepConfig lockstepConfig;
		lockstepConfig.sessionId = session.GetSessionId();
		lockstepConfig.startFrame = m_UseLobbyProtocol ? m_Lobby.GetStartFrame() : config.startFrame;
		lockstepConfig.inputDelayFrames = m_MatchConfig.inputDelayFrames;
		lockstepConfig.timeoutMs = config.lockstepWaitMs;
		lockstepConfig.localPeerId = LocalPeerFor(config.host);
		lockstepConfig.remotePeerId = RemotePeerFor(config.host);
		lockstepConfig.peerCount = m_MatchConfig.peerCount;
		lockstepConfig.remoteTransportPeerId = session.GetRemoteTransportPeerId();
		lockstepConfig.frameLane = NetTransportLane::ControlReliable;
		lockstepConfig.scenario = config.scenario;
		lockstepConfig.ownershipPolicy = NetMatchConfigUtil::OwnershipPolicyName(m_MatchConfig.ownershipPolicy);
		lockstepConfig.matchConfig = m_MatchConfig;
		if (!coordinator.Start(transport, lockstepConfig, error)) {
			SetFailed(error ? *error : "lockstep start failed");
			return false;
		}
		return true;
	}

	bool NetMatchRunner::WaitForLockstepRunning(NetLockstepCoordinator& coordinator, uint64_t maxWaitMs, std::string* error) {
		const auto startTime = std::chrono::steady_clock::now();
		while (!coordinator.IsRunning()) {
			if (m_Config.cancelRequested && m_Config.cancelRequested->load()) {
				SetFailed("match setup canceled");
				if (error) *error = m_SetupError;
				return false;
			}
			const uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - startTime).count());
			coordinator.Tick(nowMs);
			if (coordinator.IsFailed() || coordinator.IsStopped()) {
				SetFailed(coordinator.GetStats().timeoutReason);
				if (error) *error = m_SetupError;
				return false;
			}
			if (nowMs > maxWaitMs) {
				SetFailed("timed out waiting for lockstep start");
				if (error) *error = m_SetupError;
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return true;
	}

	NetLobbySnapshot NetMatchRunner::BuildLobbySnapshot(const INetTransport& transport, const NetSession& session) const {
		NetLobbySnapshot snapshot;
		snapshot.lobbyPhase = StateName(m_State);
		snapshot.activityPreset = m_MatchConfig.activityPreset;
		snapshot.sceneName = m_MatchConfig.sceneName;
		snapshot.modeName = NetMatchConfigUtil::ModeName(m_MatchConfig.mode);
		snapshot.localReady = m_Lobby.IsLocalReady();
		snapshot.remoteReady = m_Lobby.IsRemoteReady();

		const uint8_t localId = LocalPeerFor(m_Config.host);
		const uint32_t remotePing = transport.GetPeerPingMs(session.GetRemoteTransportPeerId());
		for (const NetMatchPlayerSlot& slot: m_MatchConfig.players) {
			NetLobbyMember member;
			member.peerId = slot.peerId;
			member.team = slot.team;
			member.cpu = slot.cpu;
			member.isLocal = slot.peerId == localId;
			// The remote peer's typed name arrives via its periodic peer-state; the slot only has the default.
			member.displayName = (!member.isLocal && !m_Lobby.GetRemoteName().empty()) ? m_Lobby.GetRemoteName() : slot.displayName;
			member.ready = member.isLocal ? snapshot.localReady : snapshot.remoteReady;
			member.connected = true;
			member.pingMs = member.isLocal ? 0 : remotePing;
			snapshot.members.push_back(member);
		}
		return snapshot;
	}

	void NetMatchRunner::SetFailed(const std::string& error) {
		m_State = NetMatchRuntimeState::Failed;
		m_SetupError = error;
	}

} // namespace RTE
