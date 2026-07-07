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

		// The session assigns the host id 0 and clients 1..; lockstep peer ids are 1-based and dense.
		uint8_t LockstepPeerId(uint8_t sessionAssignedId) {
			return static_cast<uint8_t>(sessionAssignedId + 1);
		}
	}

	std::map<uint8_t, NetPeerId> NetMatchRunner::BuildRemoteTransportMap(const NetSession& session) const {
		std::map<uint8_t, NetPeerId> transports;
		for (const NetSessionPeerInfo& peer : session.GetReadyPeers()) {
			transports[LockstepPeerId(peer.assignedPeerId)] = peer.transportPeerId;
		}
		return transports;
	}

	uint8_t NetMatchRunner::LocalLockstepPeerId(const NetSession& session) const {
		return LockstepPeerId(session.GetLocalPeerId());
	}

	bool NetMatchRunner::Start(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, const NetMatchRunnerConfig& config, std::string* error) {
		m_RunStartTime = std::chrono::steady_clock::now();
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
		// The host waits for every client (peerCount-1); a client waits for the host alone.
		const uint32_t expectedReadyPeers = config.host ? static_cast<uint32_t>(m_MatchConfig.peerCount - 1) : 1U;
		if (!WaitForSessionReady(session, expectedReadyPeers, config.sessionWaitMs, error)) {
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

	bool NetMatchRunner::StartNextMatch(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, std::string* error, std::vector<uint8_t> stateToStream) {
		m_RunStartTime = std::chrono::steady_clock::now();
		const uint32_t expectedReadyPeers = m_Config.host ? static_cast<uint32_t>(m_MatchConfig.peerCount - 1) : 1U;
		if (!session.IsReady() || session.GetReadyPeerCount() < expectedReadyPeers) {
			SetFailed(std::string("session is no longer connected") + (session.HasReject() ? ": " + session.BuildRejectText() : ""));
			if (error) *error = m_SetupError;
			return false;
		}
		m_SetupError.clear();
		m_StateToStream = std::move(stateToStream);
		m_ReceivedStateBytes.clear();
		m_MatchConfigHash = NetMatchConfigUtil::HashConfig(m_MatchConfig);

		if (m_UseLobbyProtocol) {
			m_State = NetMatchRuntimeState::LobbySync;
			if (!RunLobby(transport, session, m_Config.lobbyWaitMs, error)) {
				return false;
			}
			if (m_Config.postLobbySettleMs > 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(m_Config.postLobbySettleMs));
			}
		}

		m_State = NetMatchRuntimeState::LockstepStarting;
		if (!StartLockstep(transport, session, coordinator, m_Config, error) || !WaitForLockstepRunning(coordinator, m_Config.lockstepWaitMs, error)) {
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

	bool NetMatchRunner::WaitForSessionReady(NetSession& session, uint32_t expectedReadyPeers, uint64_t maxWaitMs, std::string* error) {
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
			// N-peer: the host must have every client Ready, not just the first to connect.
			if (session.IsReady() && session.GetReadyPeerCount() >= expectedReadyPeers) {
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

	bool NetMatchRunner::RunLobby(INetTransport& transport, NetSession& session, uint64_t maxWaitMs, std::string* error) {
		if (m_Config.host) {
			// The roster carries each client's session-handshake name to every peer via the config sync.
			for (const NetSessionPeerInfo& peer : session.GetReadyPeers()) {
				for (NetMatchPlayerSlot& slot : m_MatchConfig.players) {
					if (slot.peerId == LockstepPeerId(peer.assignedPeerId) && !peer.displayName.empty()) {
						slot.displayName = peer.displayName;
					}
				}
			}
		}
		NetLobbySessionConfig lobbyConfig;
		lobbyConfig.host = m_Config.host;
		lobbyConfig.localPeerId = LocalLockstepPeerId(session);
		lobbyConfig.remoteTransportPeerIds = BuildRemoteTransportMap(session);
		lobbyConfig.matchConfig = m_MatchConfig;
		lobbyConfig.startFrame = m_Config.startFrame;
		lobbyConfig.displayName = m_Config.sessionConfig.displayName;
		lobbyConfig.platform = m_Config.sessionConfig.localIdentity.platform;
		lobbyConfig.autoReady = m_Config.autoReady;
		lobbyConfig.autoStart = m_Config.autoStart;
		// A client's lobby hears nothing until the last peer arrives and the host starts its round —
		// silence is not death here. Transport disconnects still abort it immediately.
		lobbyConfig.timeoutMs = static_cast<uint32_t>(maxWaitMs);
		if (!m_Lobby.Start(transport, lobbyConfig, error)) {
			SetFailed(error ? *error : "lobby start failed");
			return false;
		}
		// A resync round streams the host's match state; the Start queues behind the last chunk.
		if (m_Config.host && !m_StateToStream.empty()) {
			m_Lobby.BeginStateTransfer(std::move(m_StateToStream));
			m_StateToStream.clear();
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
			const auto now = std::chrono::steady_clock::now();
			const uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count());
			m_Lobby.Tick(nowMs);
			// Keep session heartbeats flowing while the lobby owns the event queue: an N-peer host is
			// still session-waiting for the other clients and would otherwise declare us dead. The
			// keepalive rides the continuous run clock so the session clock never rewinds.
			session.TickKeepalive(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - m_RunStartTime).count()));
			if (m_Config.publishLobby) {
				m_Config.publishLobby(BuildLobbySnapshot(transport, session));
			}
			if (m_Lobby.IsStarted()) {
				m_MatchConfig = m_Lobby.GetMatchConfig();
				m_MatchConfigHash = m_Lobby.GetMatchConfigHash();
				if (m_Lobby.HasCompleteStateTransfer()) {
					m_ReceivedStateBytes = m_Lobby.TakeReceivedState();
				}
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
		lockstepConfig.timeoutMs = config.missingFrameGraceMs;
		lockstepConfig.localPeerId = LocalLockstepPeerId(session);
		lockstepConfig.peerCount = m_MatchConfig.peerCount;
		lockstepConfig.remoteTransportPeerIds = BuildRemoteTransportMap(session);
		// Host-star: the host relays each client's frames/checksums to the other clients.
		lockstepConfig.relayToOtherPeers = config.host;
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
		// A client adopts the host's roster mid-round; read it from the live lobby so the member
		// list grows to the real player count instead of the local placeholder config's.
		const NetMatchConfig& rosterConfig = m_Lobby.GetState() != NetLobbyState::Idle ? m_Lobby.GetMatchConfig() : m_MatchConfig;
		NetLobbySnapshot snapshot;
		snapshot.lobbyPhase = StateName(m_State);
		snapshot.activityPreset = rosterConfig.activityPreset;
		snapshot.sceneName = rosterConfig.sceneName;
		snapshot.modeName = NetMatchConfigUtil::ModeName(rosterConfig.mode);
		snapshot.localReady = m_Lobby.IsLocalReady();
		snapshot.remoteReady = m_Lobby.IsRemoteReady();

		const uint8_t localId = LocalLockstepPeerId(session);
		const std::map<uint8_t, NetPeerId> remoteTransports = BuildRemoteTransportMap(session);
		for (const NetMatchPlayerSlot& slot: rosterConfig.players) {
			NetLobbyMember member;
			member.peerId = slot.peerId;
			member.team = slot.team;
			member.cpu = slot.cpu;
			member.isLocal = slot.peerId == localId;
			// The remote peer's typed name arrives via its periodic peer-state; the slot only has the default.
			const std::string& remoteName = m_Lobby.GetRemoteName(slot.peerId);
			member.displayName = (!member.isLocal && !remoteName.empty()) ? remoteName : slot.displayName;
			member.ready = member.isLocal ? snapshot.localReady : m_Lobby.IsRemoteReady(slot.peerId);
			// Host-star: the host has a transport for every client; a client sees its SIBLINGS through
			// the host's peer-state relay, with the host's measured ping standing in for theirs.
			const auto transportIt = remoteTransports.find(slot.peerId);
			member.connected = member.isLocal || slot.cpu || transportIt != remoteTransports.end() || m_Lobby.HasHeardFrom(slot.peerId);
			if (member.isLocal) {
				member.pingMs = 0;
			} else if (transportIt != remoteTransports.end()) {
				member.pingMs = transport.GetPeerPingMs(transportIt->second);
			} else {
				member.pingMs = m_Lobby.GetRemotePingMs(slot.peerId);
			}
			snapshot.members.push_back(member);
		}
		return snapshot;
	}

	void NetMatchRunner::SetFailed(const std::string& error) {
		m_State = NetMatchRuntimeState::Failed;
		m_SetupError = error;
	}

} // namespace RTE
