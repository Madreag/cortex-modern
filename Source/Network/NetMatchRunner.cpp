#include "NetMatchRunner.h"

#include "NetIdentity.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
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
		m_Config = config;
		m_UseLobbyProtocol = config.useLobbyProtocol;
		m_ResyncRound = false;
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
		if (!WaitForSessionReady(transport, session, expectedReadyPeers, config.sessionWaitMs, error)) {
			return false;
		}
		m_MatchConfig.sessionId = session.GetSessionId();
		// High ping self-pays: each sender's delay covers its OWN round trip to the host (the
		// receive side settles by running that leg behind), so one slow link no longer delays
		// every player's input. The manual setting stays the floor for every peer.
		if (config.host && config.autoInputDelay) {
			const double tickMs = 1000.0 / 30.0;
			const uint16_t floorDelay = m_MatchConfig.inputDelayFrames;
			std::vector<uint16_t> delays(m_MatchConfig.peerCount, std::max<uint16_t>(floorDelay, 1));
			for (const auto& [peerId, transportId]: BuildRemoteTransportMap(session)) {
				const uint32_t rttMs = transport.GetPeerPingMs(transportId);
				const uint16_t neededDelay = static_cast<uint16_t>(std::min<uint32_t>(
				    static_cast<uint32_t>(std::ceil(rttMs / tickMs)) + 1U, NetMatchConfigUtil::c_MaxInputDelayFrames));
				delays[peerId - 1] = std::max(delays[peerId - 1], neededDelay);
				std::cout << "[net-match] auto input delay: peer " << static_cast<int>(peerId) << " rtt " << rttMs
				          << "ms -> " << delays[peerId - 1] << " frames (manual floor " << floorDelay << ")" << std::endl;
			}
			m_MatchConfig.peerInputDelayFrames = std::move(delays);
		}
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

	bool NetMatchRunner::PrepareRematchRoster(NetSession& session, const std::vector<uint8_t>& survivingPeerIds, std::string* error) {
		auto refuse = [&](const std::string& reason) {
			SetFailed(reason);
			if (error) *error = m_SetupError;
			return false;
		};
		std::vector<uint8_t> survivors;
		if (m_Config.host) {
			// The host's roster is the peers it can still run a lobby with; a joiner on a provisional id
			// past the roster is not one of them.
			survivors.push_back(m_MatchConfig.hostPeerId);
			for (const NetSessionPeerInfo& peer : session.GetReadyPeers()) {
				const uint8_t peerId = LockstepPeerId(peer.assignedPeerId);
				if (peerId != m_MatchConfig.hostPeerId && peerId <= m_MatchConfig.peerCount) {
					survivors.push_back(peerId);
				}
			}
		} else {
			survivors = survivingPeerIds;
		}
		std::map<uint8_t, uint8_t> seatMap;
		std::string deriveError;
		if (!NetMatchConfigUtil::DeriveRematchConfig(m_MatchConfig, survivors, m_RematchConfig, &seatMap, &deriveError)) {
			return refuse("rematch roster: " + deriveError);
		}
		m_RematchRound = true;
		if (NetMatchConfigUtil::HashConfig(m_RematchConfig) == NetMatchConfigUtil::HashConfig(m_MatchConfig)) {
			return true;
		}
		if (m_Config.host) {
			NetReconnectHost* admission = session.GetReconnectHost();
			std::vector<NetH4Seat> seats;
			if (admission) {
				// The plane's seat, not the players index, is the one a survivor's ticket names.
				std::vector<NetH4Seat> held = admission->GetSeatTable();
				for (const NetMatchPlayerSlot& was : m_MatchConfig.players) {
					uint8_t peerId = was.peerId;
					if (!was.cpu) {
						const auto moved = seatMap.find(was.peerId);
						if (moved == seatMap.end()) {
							continue;
						}
						peerId = moved->second;
					}
					const auto current = std::find_if(held.begin(), held.end(), [&was](const NetH4Seat& seat) {
						return seat.cpu == was.cpu && (was.cpu ? seat.team == static_cast<int32_t>(was.team) : seat.lockstepPeerId == was.peerId);
					});
					if (current == held.end()) {
						return refuse("rematch roster: the admission plane holds no seat for peer " + std::to_string(was.peerId));
					}
					NetH4Seat seat = *current;
					held.erase(current);
					seat.peerId = peerId > 0 ? static_cast<uint8_t>(peerId - 1) : 0;
					seat.team = static_cast<int32_t>(was.team);
					seat.lockstepPeerId = peerId;
					seat.local = !was.cpu && peerId == m_RematchConfig.hostPeerId;
					seats.push_back(seat);
				}
			}
			std::map<uint8_t, uint8_t> reseated;
			for (const auto& [seated, moved] : seatMap) {
				if (seated != m_MatchConfig.hostPeerId) {
					reseated[static_cast<uint8_t>(seated - 1)] = static_cast<uint8_t>(moved - 1);
				}
			}
			std::string seatError;
			if (!session.RenumberReadySeats(reseated, &seatError)) {
				return refuse("rematch roster: " + seatError);
			}
			if (admission) {
				admission->SetSeatTable(std::move(seats), m_RematchConfig.mode);
			}
		} else {
			const auto mine = seatMap.find(LocalLockstepPeerId(session));
			if (mine == seatMap.end()) {
				return refuse("rematch roster: this peer has no seat in the roster it derived");
			}
			std::string seatError;
			if (!session.AdoptRematchPeerId(static_cast<uint8_t>(mine->second - 1), &seatError)) {
				return refuse("rematch roster: " + seatError);
			}
		}
		m_MatchConfig = m_RematchConfig;
		m_Config.matchConfig = m_RematchConfig;
		return true;
	}

	bool NetMatchRunner::RematchRostersAgree(const NetMatchConfig& proposed, const NetMatchConfig& derived) {
		auto seatsOf = [](const NetMatchConfig& config) {
			std::vector<std::array<uint8_t, 3>> seats;
			for (const NetMatchPlayerSlot& slot : config.players) {
				seats.push_back({slot.peerId, slot.team, static_cast<uint8_t>(slot.cpu ? 1 : 0)});
			}
			std::sort(seats.begin(), seats.end());
			return seats;
		};
		return proposed.peerCount == derived.peerCount && proposed.hostPeerId == derived.hostPeerId &&
		       proposed.dedicated == derived.dedicated && seatsOf(proposed) == seatsOf(derived);
	}

	bool NetMatchRunner::VerifyRematchProposal(std::string* error) {
		if (m_Config.host || !m_RematchRound) {
			return true;
		}
		if (RematchRostersAgree(m_MatchConfig, m_RematchConfig)) {
			return true;
		}
		SetFailed("rematch roster refused: the host proposed peer_count " + std::to_string(m_MatchConfig.peerCount) + " (" +
		          HashText(NetMatchConfigUtil::HashConfig(m_MatchConfig)) + "), this peer derived peer_count " +
		          std::to_string(m_RematchConfig.peerCount) + " (" + HashText(NetMatchConfigUtil::HashConfig(m_RematchConfig)) + ")");
		if (error) *error = m_SetupError;
		return false;
	}

	bool NetMatchRunner::StartNextMatch(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, std::string* error, std::vector<uint8_t> stateToStream) {
		m_SetupError.clear();
		m_ResyncRound = !stateToStream.empty();
		m_RematchRound = false;
		// A rematch re-forms the roster on the peers still here; a resync must keep the one its snapshot
		// was taken on. The host's resync is the round that carries the state out.
		std::vector<uint8_t> rematchRoster;
		rematchRoster.swap(m_RematchRoster);
		if (!m_ResyncRound && (m_Config.host || !rematchRoster.empty()) && !PrepareRematchRoster(session, rematchRoster, error)) {
			return false;
		}
		const uint32_t expectedReadyPeers = m_Config.host ? static_cast<uint32_t>(m_MatchConfig.peerCount - 1) : 1U;
		if (!session.IsReady() || session.GetReadyPeerCount() < expectedReadyPeers) {
			SetFailed(std::string("session is no longer connected") + (session.HasReject() ? ": " + session.BuildRejectText() : ""));
			if (error) *error = m_SetupError;
			return false;
		}
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
		if (!VerifyRematchProposal(error)) {
			return false;
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

	bool NetMatchRunner::WaitForSessionReady(INetTransport& transport, NetSession& session, uint32_t expectedReadyPeers, uint64_t maxWaitMs, std::string* error) {
		const auto startTime = std::chrono::steady_clock::now();
		uint64_t nextRetryMs = 0;
		while (true) {
			if (m_Config.cancelRequested && m_Config.cancelRequested->load()) {
				SetFailed("match setup canceled");
				if (error) *error = m_SetupError;
				return false;
			}
			const uint64_t waitMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - startTime).count());
			const uint64_t nowMs = m_Config.nowMs ? m_Config.nowMs() : waitMs;
			session.Tick(nowMs);
			if (m_Config.publishLobby) {
				m_Config.publishLobby(BuildLobbySnapshot(transport, session));
			}
			// N-peer: the host must have every client Ready, not just the first to connect.
			if (session.IsReady() && session.GetReadyPeerCount() >= expectedReadyPeers) {
				return true;
			}
			// A reconnect can knock before the host's transport notices the dead slot; retry until
			// the timeout frees it (the budget above still bounds the whole wait).
			if (!m_Config.host && session.IsRejected() && session.GetRejectReason() == NetRejectReason::SessionFull) {
				if (nowMs >= nextRetryMs) {
					nextRetryMs = nowMs + 2000;
					std::string retryError;
					NetSessionConfig retryConfig = m_Config.sessionConfig;
					(void)session.StartClient(transport, m_Config.joinAddress, std::move(retryConfig), &retryError);
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				continue;
			}
			if (session.IsRejected() || session.IsFailed() || session.IsClosed()) {
				// Surface the recorded mismatch (mod/config/version, timeout) instead of the bare state name.
				const std::string rejectText = session.BuildRejectText();
				SetFailed(!rejectText.empty() ? rejectText : std::string("session did not reach Ready; state=") + NetSession::StateName(session.GetState()));
				if (error) *error = m_SetupError;
				return false;
			}
			if (waitMs > maxWaitMs) {
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
		lobbyConfig.session = &session;
		lobbyConfig.sessionNowMs = m_Config.nowMs;
		lobbyConfig.autoInputDelay = m_Config.autoInputDelay;
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
		uint64_t transferProgress = m_Lobby.GetStateTransferProgressSerial(), lastTransferProgressMs = 0;
		while (true) {
			if (m_Config.cancelRequested && m_Config.cancelRequested->load()) {
				SetFailed("match setup canceled");
				if (error) *error = m_SetupError;
				return false;
			}
			if (m_Config.readyRequested && m_Config.readyRequested->load()) {
				m_Lobby.SetLocalReady(true);
			}
			if (m_Config.startRequested && m_Config.startRequested->exchange(false)) {
				m_Lobby.RequestStart();
			}
			const auto now = std::chrono::steady_clock::now();
			const uint64_t roundMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count());
			const NetMatchRunnerClocks clocks = ResolveRoundClocks(roundMs, static_cast<bool>(m_Config.nowMs), m_Config.nowMs ? m_Config.nowMs() : 0);
			m_Lobby.Tick(clocks.lobbyMs);
			if (const uint64_t progress = m_Lobby.GetStateTransferProgressSerial(); progress != transferProgress) {
				transferProgress = progress;
				lastTransferProgressMs = clocks.budgetMs;
			}
			// The lobby round owns the transport queue, so the plane only gets its time from here.
			session.TickAdmissionPlane(clocks.planeMs);
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
			if (clocks.budgetMs >= lastTransferProgressMs && clocks.budgetMs - lastTransferProgressMs > maxWaitMs) {
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
		lockstepConfig.resumeFromSnapshot = m_ResyncRound || !m_ReceivedStateBytes.empty();
		if (const auto* admission = session.GetReconnectHost()) {
			lockstepConfig.seatPresenceEpoch = admission->GetEpoch();
		} else if (const auto* admission = session.GetReconnectClient(); admission && admission->IsAdmitted() && admission->HasRecord()) {
			lockstepConfig.seatPresenceEpoch = admission->GetRecord().epoch;
		}
		lockstepConfig.startFrame = m_UseLobbyProtocol ? m_Lobby.GetStartFrame() : config.startFrame;
		lockstepConfig.localPeerId = LocalLockstepPeerId(session);
		lockstepConfig.inputDelayFrames = NetMatchConfigUtil::PeerInputDelay(m_MatchConfig, lockstepConfig.localPeerId);
		if (!m_MatchConfig.peerInputDelayFrames.empty()) {
			for (uint8_t peerId = 1; peerId <= m_MatchConfig.peerCount; ++peerId) {
				lockstepConfig.peerInputDelayFrames[peerId] = NetMatchConfigUtil::PeerInputDelay(m_MatchConfig, peerId);
			}
		}
		lockstepConfig.timeoutMs = config.missingFrameGraceMs;
		lockstepConfig.peerCount = m_MatchConfig.peerCount;
		lockstepConfig.remoteTransportPeerIds = BuildRemoteTransportMap(session);
		// Host-star: the host relays each client's frames/checksums to the other clients.
		lockstepConfig.relayToOtherPeers = config.host;
		lockstepConfig.frameLane = NetTransportLane::ControlReliable;
		lockstepConfig.scenario = config.scenario;
		lockstepConfig.ownershipPolicy = NetMatchConfigUtil::OwnershipPolicyName(m_MatchConfig.ownershipPolicy);
		lockstepConfig.matchConfig = m_MatchConfig;
		// The host tags each round so a late packet from the previous round cannot join this one.
		if (config.host) {
			std::random_device entropy;
			do {
				lockstepConfig.roundId = (static_cast<uint64_t>(entropy()) << 32) ^ static_cast<uint64_t>(entropy()) ^
				                         static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
			} while (lockstepConfig.roundId == 0);
		}
		if (!coordinator.Start(transport, lockstepConfig, error)) {
			SetFailed(error ? *error : "lockstep start failed");
			return false;
		}
		return true;
	}

	bool NetMatchRunner::WaitForLockstepRunning(NetLockstepCoordinator& coordinator, uint64_t maxWaitMs, std::string* error) {
		// The handshake and the round must feed the coordinator ONE clock, or its per-peer liveness
		// and retransmit timers see time run backwards at the handoff into the sim loop.
		const uint64_t startMs = NetLockstepNowMs();
		while (!coordinator.IsRunning()) {
			if (m_Config.cancelRequested && m_Config.cancelRequested->load()) {
				SetFailed("match setup canceled");
				if (error) *error = m_SetupError;
				return false;
			}
			const uint64_t nowMs = NetLockstepNowMs();
			coordinator.Tick(nowMs);
			if (coordinator.IsFailed() || coordinator.IsStopped()) {
				SetFailed(coordinator.GetStats().timeoutReason);
				if (error) *error = m_SetupError;
				return false;
			}
			if (nowMs - startMs > maxWaitMs) {
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
		if (m_Config.host && session.HasReject() && session.GetReadyPeerCount() < m_Config.sessionConfig.maxPeers) {
			snapshot.errorText = "A player could not join: " + session.BuildRejectText();
		}

		const uint8_t localId = LocalLockstepPeerId(session);
		const std::map<uint8_t, NetPeerId> remoteTransports = BuildRemoteTransportMap(session);
		std::map<uint8_t, std::string> sessionNames;
		for (const NetSessionPeerInfo& peer: session.GetReadyPeers()) {
			sessionNames[LockstepPeerId(peer.assignedPeerId)] = peer.displayName;
		}
		for (const NetMatchPlayerSlot& slot: rosterConfig.players) {
			NetLobbyMember member;
			member.peerId = slot.peerId;
			member.team = slot.team;
			member.cpu = slot.cpu;
			member.isLocal = slot.peerId == localId;
			// The remote peer's typed name arrives via its periodic peer-state; the slot only has the default.
			const std::string& remoteName = m_Lobby.GetRemoteName(slot.peerId);
			member.displayName = (!member.isLocal && !remoteName.empty()) ? remoteName : slot.displayName;
			if (!member.isLocal && remoteName.empty() && sessionNames.contains(slot.peerId)) {
				member.displayName = sessionNames.at(slot.peerId);
			}
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
