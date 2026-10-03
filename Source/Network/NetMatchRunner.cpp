#include "NetMatchRunner.h"
#include "DiagnosticLine.h"

#include "GnsTransport.h"
#include "NetIdentity.h"
#include "NetWorldJoin.h"
#include "NetRoundStartScripts.h"
#include "TimerMan.h"

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
			if (!m_ActivePeerIds.empty() && std::find(m_ActivePeerIds.begin(), m_ActivePeerIds.end(), LockstepPeerId(peer.assignedPeerId)) == m_ActivePeerIds.end()) continue;
			transports[LockstepPeerId(peer.assignedPeerId)] = peer.transportPeerId;
		}
		return transports;
	}

	uint8_t NetMatchRunner::LocalLockstepPeerId(const NetSession& session) const {
		return LockstepPeerId(session.GetLocalPeerId());
	}

	bool NetMatchRunner::Start(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, const NetMatchRunnerConfig& config, std::string* error) {
		m_Config = config;
		m_HostOptionsRefused = false;
		m_UseLobbyProtocol = config.useLobbyProtocol;
		// A round opened on a checkpoint resumes from a snapshot exactly as a healed round does.
		// A round's start scripts ride the same stream as a match image, but the round starts fresh.
		m_ResyncRound = !m_StateToStream.empty() && !IsRoundStartScriptBlob(m_StateToStream);
		m_MatchConfig = config.matchConfig;
		m_ActivePeerIds = m_MatchConfig.activePeerIds;
		m_MatchConfigHash = NetMatchConfigUtil::HashConfig(m_MatchConfig);
		m_SetupError.clear();
		m_WorldJoinImage = false;
		m_ReceivedStateBytes.clear();
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
		// A one-peer roster has no remote to wait for: the host is the whole round.
		sessionConfig.readyWithoutPeers = config.host && (m_MatchConfig.peerCount == 1 || m_MatchConfig.persistentWorld);
		const bool sessionStarted = config.host
			? session.StartHost(transport, std::move(sessionConfig), error)
			: session.StartClient(transport, config.joinAddress, std::move(sessionConfig), error);
		if (!sessionStarted) {
			SetFailed(error ? *error : "session start failed");
			return false;
		}
		// The host waits for every client (peerCount-1); a client waits for the host alone.
		const uint32_t expectedReadyPeers = sessionConfig.readyWithoutPeers ? 0U : (config.host ? static_cast<uint32_t>(m_MatchConfig.peerCount - 1) : 1U);
		if (!WaitForSessionReady(transport, session, expectedReadyPeers, config.sessionWaitMs, error)) {
			return false;
		}
		m_MatchConfig.sessionId = session.GetSessionId();
		// Each sender covers its round trip through the active transport.
		if (config.host && config.autoInputDelay) {
			const double tickMs = g_TimerMan.GetDeltaTimeMS();
			const uint16_t floorDelay = m_MatchConfig.inputDelayFrames;
			std::vector<uint16_t> delays(m_MatchConfig.peerCount, std::max<uint16_t>(floorDelay, 1));
			uint16_t slowestRemoteDelay = delays.front();
			const uint32_t margin = NetMatchConfigUtil::HoldMarginFrames(m_MatchConfig);
			for (const auto& [peerId, transportId]: BuildRemoteTransportMap(session)) {
				const uint32_t rttMs = transport.GetPeerPingMs(transportId);
				NetInputDelayEstimator estimate;
				estimate.Observe(0, rttMs);
				const uint16_t neededDelay = static_cast<uint16_t>(std::min<uint32_t>(
				    estimate.RequiredFrames(tickMs, floorDelay) + margin, NetMatchConfigUtil::c_MaxInputDelayFrames));
				delays[peerId - 1] = std::max(delays[peerId - 1], neededDelay);
				slowestRemoteDelay = std::max(slowestRemoteDelay, neededDelay);
				DiagnosticLine() << "[net-match] auto input delay: peer " << static_cast<int>(peerId) << " rtt " << rttMs
				          << "ms -> " << delays[peerId - 1] << " frames (manual floor " << floorDelay << ")" << std::endl;
			}
			// The host's sender window covers the slowest link so its frames do not feed a peer's wait back into its stream.
			const size_t hostIndex = m_MatchConfig.hostPeerId > 0 && m_MatchConfig.hostPeerId <= m_MatchConfig.peerCount ? m_MatchConfig.hostPeerId - 1 : 0;
			if (delays[hostIndex] < slowestRemoteDelay) {
				delays[hostIndex] = slowestRemoteDelay;
				DiagnosticLine() << "[net-match] auto input delay: host sender window " << delays[hostIndex]
				          << " frames (slowest remote link)" << std::endl;
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

		if (m_WorldJoinImage) {
			m_State = NetMatchRuntimeState::Running;
			return true;
		}

		m_State = NetMatchRuntimeState::LockstepStarting;
		if (!StartLockstep(transport, session, coordinator, config, error) || !WaitForLockstepRunning(coordinator, config.lockstepWaitMs, error, &session)) {
			return false;
		}
		m_State = NetMatchRuntimeState::Running;
		return true;
	}

	bool NetMatchRunner::AdoptStagedHostOptions(std::string* error) {
		if (!m_Config.host || !m_Config.hostOptions) return true;
		NetHostOptionsSlot& slot = *m_Config.hostOptions;
		std::lock_guard<std::mutex> lock(slot.mutex);
		if (!slot.pending.load(std::memory_order_acquire)) return true;
		auto refuse = [&](const std::string& reason) {
			slot.refusal = "Host options refused: " + reason;
			m_HostOptionsRefused = true;
			SetFailed(slot.refusal);
			if (error) *error = slot.refusal;
			return false;
		};
		// Keep the peek and take under one lock so a newer Apply cannot replace the validated draft.
		if (slot.config.sessionId != m_MatchConfig.sessionId) return refuse("the draft names another session");
		if (slot.config.configRevision <= m_MatchConfig.configRevision) return refuse("the draft names a stale configuration revision");
		std::string validation;
		if (!NetMatchConfigUtil::ValidateLocalAlpha(slot.config, &validation)) return refuse(validation);
		NetMatchConfig staged;
		slot.TakeLocked(staged);
		m_MatchConfig = staged;
		m_Config.matchConfig = staged;
		m_MatchConfigHash = NetMatchConfigUtil::HashConfig(m_MatchConfig);
		SyncSeatingWaitToConfig();
		return true;
	}

	void NetMatchRunner::SyncSeatingWaitToConfig() {
		// A round the host gave no seating policy keeps budgeting by its message deadline; one that
		// has a policy follows the published value, so an edited idle wait takes effect at once.
		if (!m_Config.host || !m_Config.lobbySeatingWaitMs) {
			return;
		}
		m_Config.lobbySeatingWaitMs = m_MatchConfig.idleWaitMinutes > 0
		                                  ? static_cast<uint32_t>(m_MatchConfig.idleWaitMinutes) * 60000
		                                  : 0u;
	}

	bool NetMatchRunner::PrepareRematchRoster(NetSession& session, std::string* error) {
		if (m_ActiveHostPeerId != 0)
			m_MatchConfig.hostPeerId = m_ActiveHostPeerId;
		auto refuse = [&](const std::string& reason) {
			SetFailed(reason);
			if (error) *error = m_SetupError;
			return false;
		};
		std::string deriveError;
		if (!NetMatchConfigUtil::DeriveRematchConfig(m_MatchConfig, m_RematchConfig, &deriveError)) {
			return refuse("rematch roster: " + deriveError);
		}
		m_RematchRound = true;
		if (m_Config.host) {
			// Nothing has polled the transport since the round stopped, and it may know of a drop.
			session.Tick(m_Config.nowMs ? m_Config.nowMs() : session.GetClockMs());
			// The seat roster forms the round: its seats at the start on a live link are the members, and a seat whose player is away
			// or that the host opened starts held.
			std::vector<uint8_t> waitsOn;
			if (NetReconnectHost* admission = session.GetReconnectHost()) {
				admission->FormRematch();
				waitsOn = admission->StartMembers();
				if (waitsOn.empty()) DiagnosticLine() << "[net-match] the seat roster formed no rematch; its members are the ready links" << std::endl;
			}
			// With no admission plane nobody can return to a seat: the ready links are the members.
			if (waitsOn.empty())
				for (const NetSessionPeerInfo& peer : session.GetReadyPeers()) waitsOn.push_back(LockstepPeerId(peer.assignedPeerId));
			const std::vector<uint8_t> present = RematchMembers(m_RematchConfig.hostPeerId, m_RematchConfig.peerCount, waitsOn);
			if (present.size() < m_RematchConfig.peerCount) m_RematchConfig.activePeerIds = present;
		}
		if (NetMatchConfigUtil::HashConfig(m_RematchConfig) == NetMatchConfigUtil::HashConfig(m_MatchConfig)) {
			return true;
		}
		m_MatchConfig = m_RematchConfig;
		m_Config.matchConfig = m_RematchConfig;
		m_ActiveHostPeerId = m_RematchConfig.hostPeerId;
		// The host's active members stand; a client takes the host's from the lobby's agreed config.
		m_ActivePeerIds = m_Config.host ? m_RematchConfig.activePeerIds : std::vector<uint8_t>{};
		return true;
	}

	std::vector<uint8_t> NetMatchRunner::SettledRoundMembers(bool lobbyAgreed, const std::vector<uint8_t>& formed, const NetMatchConfig& agreed) {
		// A seat whose link drops after the agreement stays a member and its start holds it, so every peer keeps one member set.
		return lobbyAgreed ? agreed.activePeerIds : formed;
	}

	void NetMatchRunner::AdoptHostMigration(const NetHostMigrationResult& result, uint8_t localPeerId) {
		m_Config.host = result.hostPeerId == localPeerId;
		m_ActiveHostPeerId = result.hostPeerId;
		m_ActivePeerIds = result.members;
		m_Config.startFrame = result.boundary + 1;
		m_SnapshotProviderPeerId = result.snapshotProviderPeerId;
		m_State = NetMatchRuntimeState::Running;
	}

	std::vector<uint8_t> NetMatchRunner::RematchMembers(uint8_t hostPeerId, uint8_t peerCount, const std::vector<uint8_t>& presentPeerIds) {
		std::vector<uint8_t> present{hostPeerId};
		for (const uint8_t peer: presentPeerIds)
			if (peer != 0 && peer <= peerCount && std::find(present.begin(), present.end(), peer) == present.end()) present.push_back(peer);
		std::sort(present.begin(), present.end());
		return present;
	}

	bool NetMatchRunner::RematchKeepsEverySeat(const NetMatchConfig& proposed, const NetMatchConfig& derived, std::string* reason) {
		auto refuse = [reason](const char* why) {
			if (reason) *reason = why;
			return false;
		};
		// The hub is this peer's round authority for every seat view; it is not the host's to move.
		if (proposed.hostPeerId != derived.hostPeerId) {
			return refuse("the host proposed a different hub");
		}
		if (proposed.dedicated != derived.dedicated) {
			return refuse("the host proposed a dedicated flag this peer did not derive");
		}
		if (proposed.peerCount != derived.peerCount || proposed.players.size() != derived.players.size()) {
			return refuse("the host proposed another number of seats");
		}
		for (size_t index = 0; index < proposed.players.size(); ++index) {
			const NetMatchPlayerSlot& offered = proposed.players[index];
			const NetMatchPlayerSlot& known = derived.players[index];
			if (offered.cpu != known.cpu || offered.peerId != known.peerId || offered.team != known.team) {
				return refuse("the host proposed a seat with another id, team or kind");
			}
		}
		for (const uint8_t peer: proposed.activePeerIds) {
			if (peer == 0 || peer > proposed.peerCount) {
				return refuse("the host proposed a present member outside the seats");
			}
		}
		return true;
	}

	bool NetMatchRunner::VerifyRematchProposal(std::string* error) {
		if (m_Config.host || !m_RematchRound) {
			return true;
		}
		std::string reason;
		if (RematchKeepsEverySeat(m_MatchConfig, m_RematchConfig, &reason)) {
			return true;
		}
		SetFailed("rematch roster refused: " + reason + " (the host proposed peer_count " + std::to_string(m_MatchConfig.peerCount) + " " +
		          HashText(NetMatchConfigUtil::HashConfig(m_MatchConfig)) + ", this peer derived peer_count " +
		          std::to_string(m_RematchConfig.peerCount) + " " + HashText(NetMatchConfigUtil::HashConfig(m_RematchConfig)) + ")");
		if (error) *error = m_SetupError;
		return false;
	}

	bool NetMatchRunner::StartNextMatch(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, std::string* error, std::vector<uint8_t> stateToStream, std::vector<NetTransportEvent> pendingLobbyEvents) {
		m_PrivateJoinConfig.reset();
		m_WorldJoinStarting = false;
		m_HostLostDuringSetup = false;
		m_HostOptionsRefused = false;
		m_SetupError.clear();
		m_ResyncRound = !stateToStream.empty() || m_SnapshotProviderPeerId != 0;
		if (m_ResyncRound && coordinator.UsesBoundedWait() && !m_MatchConfig.persistentWorld) {
			m_ActivePeerIds = coordinator.ResumePeerIds();
			m_MatchConfig.activePeerIds = m_ActivePeerIds;
		}
		m_RematchRound = false;
		// A rematch keeps every seat; a resync must keep the roster its snapshot was taken on. The host's resync is the round that carries the state out.
		const bool rematchOwed = std::exchange(m_RematchOwed, false);
		// The rematch is played on the options the host staged while the last round's lobby was up. A
		// resync keeps the running world's config instead: its snapshot was taken on that one.
		if (!m_ResyncRound && !AdoptStagedHostOptions(error)) {
			return false;
		}
		if (!m_ResyncRound && (m_Config.host || rematchOwed) && !PrepareRematchRoster(session, error)) {
			return false;
		}
		const uint32_t expectedReadyPeers = m_Config.host ? static_cast<uint32_t>((m_ActivePeerIds.empty() ? m_MatchConfig.peerCount : m_ActivePeerIds.size()) - 1) : 1U;
		if (!session.IsReady() || session.GetReadyPeerCount() < expectedReadyPeers) {
			m_HostLostDuringSetup = !m_Config.host && session.IsClosed() && !session.HasReject();
			SetFailed(std::string("session is no longer connected") + (session.HasReject() ? ": " + session.BuildRejectText() : ""));
			if (error) *error = m_SetupError;
			return false;
		}
		m_StateToStream = std::move(stateToStream);
		m_ReceivedStateBytes.clear();
		m_MatchConfigHash = NetMatchConfigUtil::HashConfig(m_MatchConfig);

		if (m_UseLobbyProtocol) {
			m_State = NetMatchRuntimeState::LobbySync;
			if (!RunLobby(transport, session, m_Config.lobbyWaitMs, error, std::move(pendingLobbyEvents))) {
				return false;
			}
			if (m_Config.postLobbySettleMs > 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(m_Config.postLobbySettleMs));
			}
		}
		if (!VerifyRematchProposal(error)) {
			return false;
		}
		// The round starts only on the seat roster the host agreed it on: a peer that heard another is refused by name.
		if (!m_Config.host && m_MatchConfig.version >= NetMatchConfigUtil::c_SeatRosterVersion && m_MatchConfig.seatRosterRevision != 0) {
			std::string why;
			if (const NetReconnectClient* reconnect = session.GetReconnectClient();
			    reconnect && !reconnect->GetRosterReplica().AgreesAt(m_MatchConfig.seatRosterRevision, m_MatchConfig.seatRosterHash, &why)) {
				SetFailed("the round's seat roster was refused: " + why);
				if (error) *error = m_SetupError;
				return false;
			}
			if (session.GetReconnectClient()) DiagnosticLine() << "[net-match] seat roster revision " << m_MatchConfig.seatRosterRevision << " agreed" << std::endl;
		}
		// A rematch's members are the host's to name: the seats it starts held are the ones the agreed config leaves out.
		const std::vector<uint8_t> formedMembers = m_ActivePeerIds;
		m_ActivePeerIds = SettledRoundMembers(m_UseLobbyProtocol, m_ActivePeerIds, m_MatchConfig);
		{
			const auto list = [](const std::vector<uint8_t>& peers) {
				std::string text;
				for (const uint8_t peer: peers) text += (text.empty() ? "" : ",") + std::to_string(peer);
				return text.empty() ? std::string("all") : text;
			};
			DiagnosticLine() << "[net-match] round members=" << list(m_ActivePeerIds) << " formed=" << list(formedMembers) << " agreed=" << list(m_MatchConfig.activePeerIds)
			          << " revision=" << m_MatchConfig.configRevision << " rematch=" << m_RematchRound << " resync=" << m_ResyncRound << std::endl;
		}
		// A seat the agreed round holds is not one of its members: its player comes back through the rejoin while the round runs.
		if (m_UseLobbyProtocol && !m_Config.host && !m_ActivePeerIds.empty() &&
		    std::find(m_ActivePeerIds.begin(), m_ActivePeerIds.end(), LocalLockstepPeerId(session)) == m_ActivePeerIds.end()) {
			SetFailed(c_SeatStartsHeld);
			if (error) *error = m_SetupError;
			return false;
		}

		m_State = NetMatchRuntimeState::LockstepStarting;
		if (!StartLockstep(transport, session, coordinator, m_Config, error) || !WaitForLockstepRunning(coordinator, m_Config.lockstepWaitMs, error, &session)) {
			return false;
		}
		m_SnapshotProviderPeerId = 0;
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
		// The setup error can be a remote lobby abort reason (SetFailed at RunLobby), so a stray byte is
		// replaced rather than thrown.
		return report.dump(-1, ' ', false, json::error_handler_t::replace);
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
			if (m_Config.pumpHost) {
				m_Config.pumpHost(session);
			}
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
					std::string address = m_Config.joinAddress;
					if (m_Config.resolveJoinAddress) {
						const std::string resolved = m_Config.resolveJoinAddress();
						if (!resolved.empty()) {
							address = resolved;
							m_Config.joinAddress = resolved;
						}
					}
					(void)session.StartClient(transport, address, std::move(retryConfig), &retryError);
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

	bool NetMatchRunner::RunLobby(INetTransport& transport, NetSession& session, uint64_t maxWaitMs, std::string* error, std::vector<NetTransportEvent> pendingEvents) {
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
		// The seat roster the round is agreed on rides in the config; a peer whose copy differs is refused at the start.
		const NetReconnectHost* admission = m_Config.host ? session.GetReconnectHost() : nullptr;
		if (admission && admission->GetRoster().revision != 0) {
			m_MatchConfig.seatRosterRevision = admission->GetRoster().revision;
			m_MatchConfig.seatRosterHash = HashRoster(admission->GetRoster());
			m_MatchConfigHash = NetMatchConfigUtil::HashConfig(m_MatchConfig);
		}
		NetLobbySessionConfig lobbyConfig;
		bool relayReady = !m_Config.host || !m_Config.relayOffer || m_Config.relayOffer(m_MatchConfig.relay);
		lobbyConfig.pendingEvents = std::move(pendingEvents);
		lobbyConfig.host = m_Config.host;
		lobbyConfig.localPeerId = LocalLockstepPeerId(session);
		lobbyConfig.remoteTransportPeerIds = BuildRemoteTransportMap(session);
		lobbyConfig.matchConfig = m_MatchConfig;
		lobbyConfig.startFrame = m_Config.startFrame;
		lobbyConfig.displayName = m_Config.sessionConfig.displayName;
		lobbyConfig.platform = m_Config.sessionConfig.localIdentity.platform;
		lobbyConfig.autoReady = m_Config.autoReady;
		lobbyConfig.autoStart = m_Config.autoStart && relayReady;
		lobbyConfig.session = &session;
		lobbyConfig.sessionNowMs = m_Config.nowMs;
		lobbyConfig.autoInputDelay = m_Config.autoInputDelay;
		// The host may have reseated a client whose own round missed a drop below its id.
		lobbyConfig.assignSeats = m_RematchRound;
		lobbyConfig.enableMigration = m_Config.enableMigration && !m_MatchConfig.dedicated && !m_MatchConfig.persistentWorld;
		lobbyConfig.migrationListenPort = m_Config.sessionConfig.port;
		lobbyConfig.migrationListenAddrs = m_Config.migrationListenAddrs;
		// The ICE route names the identity the successor's rendezvous answers as; GNS has one per process.
		for (std::string& address: lobbyConfig.migrationListenAddrs)
			if (address == NetLockstepCoordinator::c_MigrationIcePrefix) address += GnsTransport::ProcessIdentity();
		lobbyConfig.sealMigration = m_Config.sealMigration;
		lobbyConfig.openMigration = m_Config.openMigration;
		lobbyConfig.snapshotProviderPeerId = m_SnapshotProviderPeerId;
		lobbyConfig.resumeMatchId = m_Config.resumeMatchId;
		lobbyConfig.resumeTick = m_Config.resumeTick;
		lobbyConfig.resumeDigest = m_Config.resumeDigest;
		lobbyConfig.resumeSideStateHash = m_Config.resumeSideStateHash;
		lobbyConfig.resumeHeld = m_Config.resumeHeld;
		// A client's lobby hears nothing until the last peer arrives and the host starts its round —
		// silence is not death here. Transport disconnects still abort it immediately. This is the
		// technical message-hearing deadline; the host's seating policy is the budget below.
		lobbyConfig.timeoutMs = static_cast<uint32_t>(maxWaitMs);
		if (!m_Lobby.Start(transport, lobbyConfig, error)) {
			SetFailed(error ? *error : "lobby start failed");
			return false;
		}
		// A resync round streams the host's match state; the Start queues behind the last chunk.
		if ((m_Config.host || m_SnapshotProviderPeerId == LocalLockstepPeerId(session)) && !m_StateToStream.empty()) {
			m_Lobby.BeginStateTransfer(std::move(m_StateToStream));
			m_StateToStream.clear();
		}

		const auto startTime = std::chrono::steady_clock::now();
		const uint64_t roundStartSessionMs = m_Config.nowMs ? m_Config.nowMs() : 0;
		uint64_t transferProgress = m_Lobby.GetStateTransferProgressSerial(), lastTransferProgressMs = 0;
		NetMatchConfig stagedOptions;
		while (true) {
			if (m_Config.cancelRequested && m_Config.cancelRequested->load()) {
				SetFailed("match setup canceled");
				if (error) *error = m_SetupError;
				return false;
			}
			if (m_Config.readyRequested && m_Config.readyRequested->load()) {
				m_Lobby.SetLocalReady(true);
			}
			if (m_Config.host && m_Config.relayOffer) {
				NetRelayConfig offer;
				const bool wasReady = relayReady;
				relayReady = m_Config.relayOffer(offer);
				if (relayReady) {
					SetRelayOffer(offer);
					if (!wasReady && m_Config.autoStart) m_Lobby.RequestStart();
				}
			}
			if (relayReady && m_Config.startRequested && m_Config.startRequested->exchange(false)) {
				if (m_Config.host && m_Config.roundStartScripts) {
					if (std::vector<uint8_t> scripts = m_Config.roundStartScripts(); !scripts.empty()) m_Lobby.BeginStateTransfer(std::move(scripts));
				}
				m_Lobby.RequestStart();
			}
			// A new seat roster revision is the round's next configuration revision while the lobby is open, as a host option is.
			if (admission && admission->GetRoster().revision != m_MatchConfig.seatRosterRevision && !m_Lobby.IsStarted()) {
				NetMatchConfig stamped = m_MatchConfig;
				stamped.configRevision = m_MatchConfig.configRevision + 1;
				stamped.seatRosterRevision = admission->GetRoster().revision;
				stamped.seatRosterHash = HashRoster(admission->GetRoster());
				// A forming round's members are the roster's: a seat that drops or that the host opens leaves them and starts held.
				if (const std::vector<uint8_t> waitsOn = admission->StartMembers(); !waitsOn.empty()) {
					const std::vector<uint8_t> present = RematchMembers(stamped.hostPeerId, stamped.peerCount, waitsOn);
					stamped.activePeerIds = present.size() < stamped.peerCount ? present : std::vector<uint8_t>{};
				}
				std::string stampError;
				if (m_Lobby.RepublishMatchConfig(stamped, &stampError)) {
					m_MatchConfig = stamped;
					m_Config.matchConfig = stamped;
					m_MatchConfigHash = m_Lobby.GetMatchConfigHash();
				} else if (stampError != m_LastRosterStampRefusal) {
					m_LastRosterStampRefusal = stampError;
					DiagnosticLine() << "[net-match] seat roster revision " << stamped.seatRosterRevision << " not republished: " << stampError << std::endl;
				}
			}
			// An accepted host-options draft becomes this round's next configuration revision here, on
			// the thread that owns the lobby: every peer re-acknowledges it before the Start gate opens.
			if (m_Config.host && m_Config.hostOptions && m_Config.hostOptions->Take(stagedOptions)) {
				std::string republishError;
				if (m_Lobby.RepublishMatchConfig(stagedOptions, &republishError)) {
					m_MatchConfig = stagedOptions;
					m_Config.matchConfig = stagedOptions;
					m_MatchConfigHash = m_Lobby.GetMatchConfigHash();
					SyncSeatingWaitToConfig();
					DiagnosticLine() << "[net-match] host options: config revision " << m_MatchConfig.configRevision
					          << " published to every peer" << std::endl;
				} else {
					// A refused draft fails the host's transaction, never the round: the lobby keeps
					// the revision its peers have already acknowledged.
					DiagnosticLine() << "[net-match] host options refused: " << republishError << std::endl;
				}
			}
			const auto now = std::chrono::steady_clock::now();
			// The round's elapsed time is the session clock's when the service supplies one, so the
			// waits here and the admission deadlines measure the same time.
			const uint64_t sessionMs = m_Config.nowMs ? m_Config.nowMs() : 0;
			const uint64_t roundMs = m_Config.nowMs
			                             ? (sessionMs > roundStartSessionMs ? sessionMs - roundStartSessionMs : 0)
			                             : static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count());
			const NetMatchRunnerClocks clocks = ResolveRoundClocks(roundMs, static_cast<bool>(m_Config.nowMs), sessionMs);
			m_Lobby.Tick(clocks.lobbyMs);
			// A seat told its round ended while it was held or rejoining takes the end record, not a round.
			if (!m_Config.host && m_Lobby.GetRoundEndedRecord() &&
			    !(m_Lobby.HasCompleteStateTransfer() && IsWorldJoinImageBlob(m_Lobby.PeekReceivedState()))) {
				SetFailed("the round ended while this seat was held");
				if (error) *error = m_SetupError;
				return false;
			}
			if (const uint64_t progress = m_Lobby.GetStateTransferProgressSerial(); progress != transferProgress) {
				transferProgress = progress;
				lastTransferProgressMs = clocks.budgetMs;
			}
			// The lobby round owns the transport queue, so the plane only gets its time from here.
			session.TickAdmissionPlane(clocks.planeMs);
			if (m_Config.pumpHost) {
				m_Config.pumpHost(session);
			}
			if (m_Config.publishLobby) {
				m_Config.publishLobby(BuildLobbySnapshot(transport, session));
			}
			if (m_Lobby.HasCompleteStateTransfer() && IsWorldJoinImageBlob(m_Lobby.PeekReceivedState())) {
				m_MatchConfig = m_Lobby.GetMatchConfig();
				m_MatchConfigHash = m_Lobby.GetMatchConfigHash();
				m_ReceivedStateBytes = m_Lobby.TakeReceivedState();
				m_WorldJoinImage = true;
				return true;
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
				m_HostLostDuringSetup = m_Lobby.DidLoseHost();
				SetFailed(m_Lobby.GetFailureReason());
				if (error) *error = m_SetupError;
				return false;
			}
			// The seating wait is re-read every tick because a live options edit republishes it.
			if (clocks.budgetMs >= lastTransferProgressMs &&
			    SeatingWaitExpired(m_Config.lobbySeatingWaitMs, static_cast<uint32_t>(maxWaitMs), clocks.budgetMs - lastTransferProgressMs)) {
				m_HostLostDuringSetup = !m_Config.host;
				m_Lobby.TimeoutWaitingForStart();
				SetFailed(m_Lobby.GetFailureReason());
				if (error) *error = m_SetupError;
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}

	void NetMatchRunner::CarrySessionTraffic(NetLockstepCoordinator& coordinator) {
		coordinator.SetSessionEventSink([this](const NetTransportEvent& event) {
			// The lobby has ended: its late packets are not the session's.
			if (event.type == NetTransportEventType::PacketReceived && NetLobbyProtocol::Decode(event.bytes).ok) return;
			if (m_SessionTraffic.size() >= 512) {
				if (m_SessionTrafficDropped++ == 0)
					DiagnosticLine() << "[net-match] the session traffic queue is full at 512 events: dropping from transport peer " << event.peerId << std::endl;
				return;
			}
			m_SessionTraffic.push_back(event);
		});
	}

	void NetMatchRunner::DeliverSessionTraffic(NetSession& session, uint64_t nowMs) {
		while (!m_SessionTraffic.empty()) {
			const NetTransportEvent event = std::move(m_SessionTraffic.front());
			m_SessionTraffic.pop_front();
			session.InjectEvent(event, nowMs);
		}
	}

	std::vector<NetTransportEvent> NetMatchRunner::TakeSessionTraffic() {
		std::vector<NetTransportEvent> events(std::make_move_iterator(m_SessionTraffic.begin()), std::make_move_iterator(m_SessionTraffic.end()));
		m_SessionTraffic.clear();
		return events;
	}

	bool NetMatchRunner::StartLockstep(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, const NetMatchRunnerConfig& config, std::string* error) {
		NetLockstepConfig lockstepConfig;
		lockstepConfig.sessionId = session.GetSessionId();
		lockstepConfig.resumeFromSnapshot = RoundResumesASnapshot(m_ResyncRound, !m_ReceivedStateBytes.empty() && !IsRoundStartScriptBlob(m_ReceivedStateBytes),
		                                                         m_UseLobbyProtocol && m_Lobby.AnsweredResumeHeld());
		if (const auto* admission = session.GetReconnectHost()) {
			lockstepConfig.seatPresenceEpoch = admission->GetEpoch();
			for (const auto& seat: admission->GetSeatTable()) {
				NetPeerId connection = c_InvalidNetPeerId;
				uint32_t generation = 0, incarnation = 0;
				if (!seat.cpu && admission->GetSeatHolder(seat.stableSeat, connection, generation, incarnation) && incarnation != 0)
					lockstepConfig.peerIncarnations[seat.lockstepPeerId] = incarnation;
			}
		} else if (const auto* admission = session.GetReconnectClient(); admission && admission->IsAdmitted() && admission->HasRecord()) {
			lockstepConfig.seatPresenceEpoch = admission->GetRecord().epoch;
		}
		lockstepConfig.startFrame = m_UseLobbyProtocol ? m_Lobby.GetStartFrame() : config.startFrame;
		lockstepConfig.localPeerId = LocalLockstepPeerId(session);
		lockstepConfig.inputDelayFrames = NetMatchConfigUtil::PeerInputDelay(m_MatchConfig, lockstepConfig.localPeerId);
		lockstepConfig.adaptiveInputDelay = m_UseLobbyProtocol ? m_MatchConfig.delayPolicy == NetMatchDelayPolicy::Auto : config.autoInputDelay;
		lockstepConfig.simTickMs = g_TimerMan.GetDeltaTimeMS();
		lockstepConfig.initialDelaySamples = m_Lobby.GetInputDelaySamples();
		lockstepConfig.publishLiveConfig = [this](const NetMatchConfig& live) {
			m_MatchConfig = live;
			m_MatchConfigHash = NetMatchConfigUtil::HashConfig(live);
		};
		lockstepConfig.substituteSlowPeers = m_MatchConfig.version >= NetMatchConfigUtil::c_TimingOptionsVersion && m_MatchConfig.slowPlayerPolicy == NetSlowPlayerPolicy::Substitute;
		lockstepConfig.slowPlayerBoundTicks = m_MatchConfig.slowPlayerBoundTicks;
		lockstepConfig.requirePublishedStart = m_UseLobbyProtocol && !m_WorldJoinStarting;
		// The host's redundancy window rides the agreed config, so every peer repeats the same ticks.
		lockstepConfig.frameRedundancyTicks = m_MatchConfig.frameRedundancyTicks;
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
		// A world joiner's round opens inside one that has been running: the members it joins owe it
		// every frame from its own start, so none of them ramps in behind the input delay.
		lockstepConfig.joinsRunningRound = m_WorldJoinStarting;
		// Input frames ride the unreliable lane: a lost packet is repaired by the next one's window, not by a
		// retransmission every later frame waits behind.
		lockstepConfig.frameLane = NetTransportLane::InputUnreliable;
		// Peers compare the activity in the start handshake, so it comes from the adopted config like every
		// other agreed field; a joining peer's own request only carries its local default.
		lockstepConfig.scenario = m_UseLobbyProtocol ? m_MatchConfig.activityPreset : config.scenario;
		lockstepConfig.ownershipPolicy = NetMatchConfigUtil::OwnershipPolicyName(m_MatchConfig.ownershipPolicy);
		lockstepConfig.matchConfig = m_MatchConfig;
		lockstepConfig.authorityPeerId = m_ActiveHostPeerId;
		lockstepConfig.activePeerIds = m_ActivePeerIds;
		if (lockstepConfig.activePeerIds.empty()) lockstepConfig.activePeerIds = m_MatchConfig.activePeerIds;
		if (m_WorldJoinStarting) {
			const auto& previous = m_PrivateJoinConfig ? *m_PrivateJoinConfig : coordinator.GetConfig();
			lockstepConfig.migrationKey = previous.migrationKey;
			lockstepConfig.migrationGeneration = previous.migrationGeneration;
			lockstepConfig.migrationTransportFactory = previous.migrationTransportFactory;
			lockstepConfig.migrationIceDial = previous.migrationIceDial;
			lockstepConfig.migrationIceHost = previous.migrationIceHost;
		} else if (m_Config.configureMigration) {
			m_Config.configureMigration(lockstepConfig);
		}
		// The host tags each round so a late packet from the previous round cannot join this one.
		if (config.host) {
			std::random_device entropy;
			do {
				const uint64_t high = entropy();
				const uint64_t low = entropy();
				lockstepConfig.roundId = (high << 32) ^ low ^
				                         static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
			} while (lockstepConfig.roundId == 0);
		}
		if (m_WorldJoinStarting && m_PrivateJoinConfig) {
			lockstepConfig.resumeFromSnapshot = false;
			lockstepConfig.roundId = m_PrivateJoinConfig->roundId;
			lockstepConfig.migrationGeneration = m_PrivateJoinConfig->migrationGeneration;
			lockstepConfig.originalRoundConfigHash = m_PrivateJoinConfig->originalRoundConfigHash;
			lockstepConfig.initialPeerLeaves = m_PrivateJoinConfig->initialPeerLeaves;
			lockstepConfig.initialDelayChanges = m_PrivateJoinConfig->initialDelayChanges;
			lockstepConfig.initialSeatHolds = m_PrivateJoinConfig->initialSeatHolds;
			lockstepConfig.initialSeatReclaims = m_PrivateJoinConfig->initialSeatReclaims;
			lockstepConfig.seatStateThroughFrame = m_PrivateJoinConfig->seatStateThroughFrame;
			lockstepConfig.peerIncarnations = m_PrivateJoinConfig->peerIncarnations;
			lockstepConfig.activePeerIds.clear();
			for (uint8_t peer = 1; peer <= lockstepConfig.peerCount; ++peer)
				if (peer == lockstepConfig.localPeerId || !lockstepConfig.initialPeerLeaves.contains(peer)) lockstepConfig.activePeerIds.push_back(peer);
		}
		// A seat knocking to come back while the round forms is the session's traffic, never dropped for want of a reader.
		CarrySessionTraffic(coordinator);
		if (!coordinator.Start(transport, lockstepConfig, error)) {
			SetFailed(error ? *error : "lockstep start failed");
			return false;
		}
		return true;
	}

	bool NetMatchRunner::WaitForLockstepRunning(NetLockstepCoordinator& coordinator, uint64_t maxWaitMs, std::string* error, NetSession* session) {
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
			if (session) DeliverSessionTraffic(*session, m_Config.nowMs ? m_Config.nowMs() : session->GetClockMs());
			if (coordinator.IsFailed() || coordinator.IsStopped()) {
				m_HostLostDuringSetup = !m_Config.host && coordinator.GetStats().timeoutReason.starts_with("PeerDisconnected:");
				SetFailed(coordinator.GetStats().timeoutReason);
				if (error) *error = m_SetupError;
				return false;
			}
			// The initial start packets are the lobby handshake. A service match remains
			// parked until the game thread measures and republishes activity startup.
			if (coordinator.IsRunning() || (coordinator.HasReceivedAllRemoteStarts() && coordinator.GetConfig().requirePublishedStart)) {
				return true;
			}
			if (nowMs - startMs > maxWaitMs) {
				m_HostLostDuringSetup = !m_Config.host;
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
		const NetMatchConfig& rosterConfig = m_State == NetMatchRuntimeState::Running || m_Lobby.GetState() == NetLobbyState::Idle ? m_MatchConfig : m_Lobby.GetMatchConfig();
		NetLobbySnapshot snapshot;
		snapshot.hostPeerId = m_ActiveHostPeerId != 0 ? m_ActiveHostPeerId : rosterConfig.hostPeerId;
		snapshot.lobbyPhase = StateName(m_State);
		snapshot.activityPreset = rosterConfig.activityPreset;
		snapshot.activityModule = rosterConfig.activityModule;
		snapshot.sceneName = rosterConfig.sceneName;
		snapshot.sceneModule = rosterConfig.sceneModule;
		snapshot.modeName = NetMatchConfigUtil::ModeName(rosterConfig.mode);
		snapshot.modeLabel = NetMatchConfigUtil::ModeLabel(rosterConfig.mode);
		snapshot.localReady = m_Lobby.IsLocalReady();
		// A lobby that resumes a match from disk names its checkpoint, so every peer's UI can say which
		// one it stands on and whether this peer is loading its own copy.
		snapshot.resumeMatchId = m_Config.resumeMatchId;
		snapshot.resumeTick = m_Config.resumeTick;
		snapshot.resumeDigest = m_Config.resumeDigest;
		snapshot.resumeHeldLocally = m_Config.host || m_Lobby.AnsweredResumeHeld();
		// Start waits on a live remote ready, not the idle default (a reject never seats one).
		snapshot.remoteReady = m_Lobby.GetState() != NetLobbyState::Idle && m_Lobby.IsRemoteReady();
		if (m_Config.host && session.HasReject() && session.GetReadyPeerCount() < m_Config.sessionConfig.maxPeers) {
			snapshot.errorText = "A player could not join: " + session.BuildPlayerRefusalText();
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
			member.inputDelayFrames = NetMatchConfigUtil::PeerInputDelay(rosterConfig, slot.peerId);
			snapshot.members.push_back(member);
		}
		return snapshot;
	}

	bool NetMatchRunner::StartWorldJoinLockstep(INetTransport& transport, NetSession& session, NetLockstepCoordinator& coordinator, uint64_t startFrame, uint64_t updateTick, std::string* error) {
		m_Lobby.SetStartFrame(startFrame);
		m_Config.startFrame = startFrame;
		m_State = NetMatchRuntimeState::LockstepStarting;
		// The joiner handshakes while replaying toward its agreed activation frame.
		m_WorldJoinStarting = true;
		m_WorldJoinStartTicks = 0;
		m_WorldJoinStartLastTick.reset();
		if (!StartLockstep(transport, session, coordinator, m_Config, error)) {
			return false;
		}
		return PumpWorldJoinLockstepStart(coordinator, updateTick, error);
	}

	bool NetMatchRunner::PumpWorldJoinLockstepStart(NetLockstepCoordinator& coordinator, uint64_t updateTick, std::string* error) {
		if (!IsWorldJoinLockstepStarting()) {
			return m_State == NetMatchRuntimeState::Running;
		}
		if (m_WorldJoinStartLastTick != updateTick) {
			m_WorldJoinStartLastTick = updateTick;
			++m_WorldJoinStartTicks;
		}
		coordinator.Tick(NetLockstepNowMs());
		if (coordinator.IsRunning()) {
			m_State = NetMatchRuntimeState::Running;
			m_WorldJoinStarting = false;
			return true;
		}
		if (coordinator.IsFailed() || coordinator.IsStopped()) {
			m_WorldJoinStarting = false;
			SetFailed(coordinator.GetStats().timeoutReason);
			if (error) *error = m_SetupError;
			return false;
		}
		if (m_WorldJoinStartTicks > m_Config.worldJoinStartWaitTicks) {
			m_WorldJoinStarting = false;
			SetFailed("timed out waiting for lockstep start after " + std::to_string(m_WorldJoinStartTicks) + " updates");
			if (error) *error = m_SetupError;
			return false;
		}
		return false;
	}

	void NetMatchRunner::SetFailed(const std::string& error) {
		m_State = NetMatchRuntimeState::Failed;
		m_SetupError = error;
	}

} // namespace RTE
