#include "NetMatchService.h"
#include "NetA7Journal.h"

#include "ActivityMan.h"
#include "Constants.h"
#include "GameActivity.h"
#include "Scene.h"
#include "GameVersion.h"
#include "FrameMan.h"
#include "GUIInput.h"
#include "GnsTransport.h"
#include "NetHttpClient.h"
#ifdef CCCP_WITH_GNS
#include "GnsSignaling.h"
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
#endif
#include "NetIdentity.h"
#include "NetPortMap.h"
#include "NetProtocol.h"
#include "PresetMan.h"
#include "ScenarioRunner.h"
#include "SettingsMan.h"
#include "System.h"
#include "System/FaultInjection.h"
#include "TimerMan.h"
#include "UInputMan.h"

#include "MovableMan.h"

#include "nlohmann/json.hpp"

#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <iomanip>
#include <list>
#include <optional>
#include <random>
#include <string>
#include <sstream>
#include <thread>
#include <utility>

std::string BuildLoopPaceJson();

namespace RTE {

	std::string NetMatchSummary::DurationText() const {
		const uint64_t seconds = runningTicks / 60;
		std::ostringstream text;
		text << std::setfill('0') << std::setw(2) << seconds / 60 << ':' << std::setw(2) << seconds % 60;
		return text.str();
	}

	std::string NetMatchSummary::LineText() const {
		std::string text = "Last match: " + (winnerTeam < 0 ? std::string("draw") : "Team " + std::to_string(winnerTeam + 1) + " wins");
		text += " | " + DurationText() + " | ";
		for (size_t i = 0; i < peers.size(); ++i) text += (i ? ", " : "") + peers[i].name;
		std::replace_if(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; }, ' ');
		return text;
	}

	std::string NetMatchSummary::IdentityText() const {
		if (identityLine.empty()) return {};
		return identityLine + "\nSHA256: " + System::GetThisExeSha256() + "\nCodec: Controller " + std::to_string(ControllerFrame::c_Version) + " | Lockstep " + std::to_string(NetLockstepCodec::c_Version);
	}

	std::string NetMatchSummary::DetailsText() const {
		std::ostringstream text;
		text << "Result: " << result << "\nWinner: " << (winnerTeam < 0 ? "draw" : "Team " + std::to_string(winnerTeam + 1));
		text << "\nDuration: " << DurationText() << " (" << runningTicks << " ticks at 60 tps)\n\nPeers";
		for (const Peer& peer : peers) {
			text << '\n' << peer.name << " | team " << peer.team + 1 << " | seat " << peer.seat << " | delay " << peer.inputDelayFrames;
		}
		text << "\n\nResyncs: " << resyncs << " | Drops: " << drops << " | Reclaims: " << reclaims << " | Substitutions: " << substitutions;
		const auto pace = nlohmann::json::parse(paceJson, nullptr, false);
		if (pace.is_object()) {
			text << std::fixed << std::setprecision(1) << "\nFinal pace: " << pace.value("wall_tps", 0.0) << " tps, "
			     << pace.value("sim_ms_per_tick", 0.0) << " ms/tick";
		}
		text << "\n\n" << IdentityText();
		return text.str();
	}

	std::optional<NetMatchSummary> NetMatchService::GetLastMatchSummary() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_LastMatchSummary;
	}

	void NetMatchService::UpdateSummarySeatsLocked() {
		for (const auto& [peerId, seat] : m_SeatPresence.GetSeats()) {
			auto& previous = m_SummarySeats[peerId];
			const auto away = [](NetSeatPresenceState state) {
				return state == NetSeatPresenceState::Disconnected || state == NetSeatPresenceState::Reconnecting || state == NetSeatPresenceState::Left;
			};
			if (away(seat.state) && !away(previous.state)) ++m_CurrentMatchSummary.drops;
			if (previous.peerId && seat.holderGeneration > previous.holderGeneration) {
				++m_CurrentMatchSummary.substitutions;
			} else if (previous.peerId && !away(seat.state) && (away(previous.state) || seat.incarnation > previous.incarnation)) {
				++m_CurrentMatchSummary.reclaims;
			}
			for (auto& peer : m_CurrentMatchSummary.peers) {
				if (peer.peerId != peerId) continue;
				peer.seat = seat.stableSeat;
				if (!seat.holderName.empty()) peer.name = seat.holderName;
			}
			previous = seat;
		}
	}

	void NetMatchService::CaptureMatchSummaryLocked(const std::string& result) {
		// The first terminal result owns this round; Main's Complete paths quit after capturing it.
		if (m_State != NetMatchServiceState::Running || m_LastMatchSummary || m_CurrentMatchSummary.identityLine.empty()) return;
		UpdateSummarySeatsLocked();
		m_CurrentMatchSummary.result = result.empty() ? "Match complete" : result;
		const auto* activity = dynamic_cast<const GameActivity*>(g_ActivityMan.GetActivity());
		m_CurrentMatchSummary.winnerTeam = activity ? activity->GetWinnerTeam() : Activity::NoTeam;
		m_CurrentMatchSummary.runningTicks = ScenarioRunner::GetLockstepAppliedFrame();
		m_CurrentMatchSummary.paceJson = ::BuildLoopPaceJson();
		m_LastMatchSummary = m_CurrentMatchSummary;
	}

	std::string NetIceHostIdentity(const std::string& sessionId) {
		// Kept in step with GnsDirectorySignalDispatcher::HostIdentity; the selftest asserts they agree.
		constexpr size_t c_IdentityChars = 24;
		std::string digits;
		for (const char ch : sessionId) {
			if (ch != '-' && digits.size() < c_IdentityChars) {
				digits += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
		}
		return "str:h-" + digits;
	}

	std::string NetIceRowJoinMode(bool iceEnabled, bool hasDirectAddress, const std::string& boundSessionId, const std::string& rowSessionId) {
		if (!iceEnabled) {
			return "ip";
		}
		// Only the bound directory id answers on this ICE listener.
		if (!boundSessionId.empty() && boundSessionId != rowSessionId) {
			return "ip";
		}
		return hasDirectAddress ? "either" : "ice";
	}

	void FillDirectoryLocalIdentity(NetDirectoryLocalIdentity& local, const NetIdentityManifest& manifest) {
		local.networkProtocolVersion = manifest.networkProtocolVersion;
		local.lockstepCodecVersion = manifest.deterministicConfig.lockstepCodecVersion;
		local.controllerFrameVersion = manifest.controllerFrameVersion;
		local.sessionIdentityHash = NetIdentity::HashHex(manifest.sessionIdentityHash);
		local.moduleManifestHash = NetIdentity::HashHex(manifest.moduleManifestHash);
	}

	bool BuildIdentityManifest(NetIdentityManifest& manifest, std::string* error, bool world) {
		NetIdentityBuildOptions options;
		options.buildId = "stage2-p2d-local";
		options.sessionRulesTag = "stage2-p2-session-rules";
		NetIdentity::StampOptionsForTarget(options, world);
		return NetIdentity::BuildCurrentManifest(manifest, error, options);
	}

	std::string NetIceResolveSessionRow(const std::vector<NetDirectorySessionRow>& rows, const NetDirectoryLocalIdentity& local, const std::string& sessionId, NetIceJoinTarget* out, const NetDirectoryLocalIdentity* worldLocal) {
		for (const NetDirectorySessionRow& row : rows) {
			if (row.sessionId != sessionId) {
				continue;
			}
			// The join list decides joinability, so a session-id join is refused with its labels.
			const std::vector<NetDirectoryClient::GameRow> merged = NetDirectoryClient::MergeGameLists({}, {row}, local, worldLocal);
			if (merged.empty()) {
				break;
			}
			if (!merged.front().joinable) {
				return merged.front().reason.empty() ? "refused" : merged.front().reason;
			}
			if (out) {
				out->identity = NetIceHostIdentity(row.sessionId);
				out->joinMode = row.joinMode;
				out->address = merged.front().address;
				out->port = merged.front().port;
				out->persistentWorld = row.persistentWorld;
			}
			return {};
		}
		return "no such session";
	}

	bool NetMatchService::s_AdmissionEnabled = true;
	uint32_t NetMatchService::s_AutosaveSeconds = 0;
	bool NetMatchService::s_AutosaveSecondsOverridden = false;
	std::string NetMatchService::s_TicketStorePath;
	std::string NetMatchService::s_JoinWaitPath;
	bool NetMatchService::s_ApplyForSeat = false;
	uint16_t NetMatchService::s_ApplySeat = 0;
	bool NetMatchService::s_ApplyOnce = false;
	bool NetMatchService::s_AutoSubstitute = false;
	uint16_t NetMatchService::s_AutoSubstituteSeat = 0;
	uint64_t NetMatchService::s_AutoSubstituteDelayMs = 0;
	bool NetMatchService::s_AutoSubstituteThenCancel = false;

// Per-process file names: same-machine instances share Userdata, so concurrent matches must never share a snapshot file.
static std::string ResyncSaveName() {
	return "p5resync_" + std::to_string(System::GetProcessID());
}

	namespace {
		using json = nlohmann::json;

		constexpr uint64_t c_UiSessionId = 0x5354414745325034ULL;
		constexpr uint64_t c_HostNonce = 0x503441484F53544ULL;
		constexpr uint64_t c_ClientNonce = 0x503441434C49454ULL;
		constexpr uint32_t c_MenuLobbyWaitMs = 10 * 60 * 1000;

		// The host dedupes joiners by nonce, so two clients on one session must present distinct ones.
		// Transport-level identity only — the sim never sees it, so real randomness is fine here.
		json AdmissionJsonFromHost(const NetReconnectHost& host) {
			const NetReconnectHostStats& stats = host.GetStats();
			return json{
				{"new_joins", stats.newJoins},
				{"ticket_offers_sent", stats.ticketOffersSent},
				{"ticket_offer_retransmits", stats.ticketOfferRetransmits},
				{"provisional_seats_opened", stats.provisionalSeatsOpened},
				{"provisional_seats_committed", stats.provisionalSeatsCommitted},
				{"provisional_seats_expired", stats.provisionalSeatsExpired},
				{"provisional_seats_refused", stats.provisionalSeatsRefused},
				{"provisional_seats_resumed", stats.provisionalSeatsResumed},
				{"persistence_failures", stats.persistenceFailures},
				{"reclaims_accepted", stats.reclaimsAccepted},
				{"identity_rejections", stats.identityRejections},
				{"denials_scheduled", stats.denialsScheduled},
				{"denials_released", stats.denialsReleased},
				{"replayed_results", stats.replayedResults},
				{"stale_epoch_drops", stats.staleEpochDrops},
				{"unknown_transaction_drops", stats.unknownTransactionDrops},
				{"fenced_packets", stats.fencedPackets},
				{"fenced_disconnects", stats.fencedDisconnects},
				{"incarnations_bound", stats.incarnationsBound},
				{"seats_dropped", stats.seatsDropped},
				{"seats_closed_by_leave", stats.seatsClosedByLeave},
				{"ledger_drops_recorded", stats.ledgerDropsRecorded},
				{"reseats_issued", stats.reseatsIssued},
				{"reseats_without_a_ledger", stats.reseatsWithoutALedger},
				{"reseats_without_survivors", stats.reseatsWithoutSurvivors},
				{"reseat_live_on_team_not_named", stats.reseatLiveOnTeamNotNamed},
				{"reclaim_retransmits_dropped", stats.reclaimRetransmitsDropped},
				{"seat_holds_expired", stats.seatHoldsExpired},
				{"seats_released_in_lobby", stats.seatsReleasedInLobby},
				{"applicants_registered", stats.applicantsRegistered},
				{"applicants_refused", stats.applicantsRefused},
				{"applicants_expired", stats.applicantsExpired},
				{"applicants_displaced", stats.applicantsDisplaced},
				{"substitution_offers_sent", stats.substitutionOffersSent},
				{"substitution_offer_retransmits", stats.substitutionOfferRetransmits},
				{"substitutions_committed", stats.substitutionsCommitted},
				{"substitutions_cancelled", stats.substitutionsCancelled},
				{"substitutions_superseded", stats.substitutionsSuperseded},
				{"substitution_ack_failures", stats.substitutionAckFailures},
				{"reassigned_reclaims_refused", stats.reassignedReclaimsRefused},
				{"pending_applicants", 0},
			};
		}

		uint64_t MakeClientNonce() {
			std::random_device device;
			return c_ClientNonce ^ (static_cast<uint64_t>(device()) << 32) ^ static_cast<uint64_t>(device());
		}

		// Set only while PumpSessionEvents runs, which is the game thread inside the sim tick. The
		// ownership census walks g_MovableMan, so any other thread asking for one is refused.
		thread_local bool t_SimCensusOpen = false;

		struct SimCensusScope {
			SimCensusScope() { t_SimCensusOpen = true; }
			~SimCensusScope() { t_SimCensusOpen = false; }
		};

		uint64_t SteadyNowMs() {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		// The port mapper is file-scope because NetMatchService.h is outside this lane's edit set and
		// the service is a process singleton: one mapping belongs to one hosted match at a time.
		NetPortMap s_PortMap;
		bool s_PortMapRequested = false; //!< This match's host asked the router for a mapping.
		bool s_PortMapApplied = false;   //!< The row already carries the mapped external address.
		std::string s_PortMapLine;       //!< The last status line handed to a snapshot.
		uint32_t s_PortMapSerial = 0;    //!< Bumped when s_PortMapLine changes.

		std::mutex s_ObservedIpMutex;
		std::string s_DirectoryObservedIp; //!< The address the directory saw the register come from.

		/// NetDirectoryClient's register reply carries observed_ip but drops it; this transport is a
		/// pass-through NetHttpClient that copies the field out of the 200 reply so the report can
		/// show it without touching the client.
		class ObservedIpTransport final : public NetDirectoryClient::Transport {
		public:
			ObservedIpTransport(std::string baseUrl, std::string installKey, std::string certPinSha256) :
				m_BaseUrl(std::move(baseUrl)), m_CertPinSha256(std::move(certPinSha256)) {
				m_Headers = {
					{"Content-Type", "application/json"},
					{"X-Install-Key", std::move(installKey)},
				};
			}

			void Start(const NetDirectoryClient::Request& request) override {
				m_Method = request.method;
				m_Path = request.path;
				m_Client.Start(request.method, m_BaseUrl + request.path, m_Headers, request.body, m_CertPinSha256);
			}
			bool Finished() override { return m_Client.Poll() == NetHttpClient::PollResult::Done; }
			NetDirectoryClient::Reply Take() override {
				const NetHttpClient::Response response = m_Client.GetResponse();
				if (m_Method == "POST" && m_Path == "/v1/sessions" && response.statusCode == 200) {
					try {
						const json parsed = json::parse(response.body);
						if (parsed.is_object() && parsed.contains("observed_ip") && parsed["observed_ip"].is_string()) {
							std::lock_guard<std::mutex> lock(s_ObservedIpMutex);
							s_DirectoryObservedIp = parsed["observed_ip"].get<std::string>();
						}
					} catch (...) {
					}
				}
				return {response.statusCode, response.body, response.error};
			}
			void Abort() override { m_Client.Cancel(); }

		private:
			NetHttpClient m_Client;
			std::string m_BaseUrl;
			std::string m_CertPinSha256;
			std::vector<std::pair<std::string, std::string>> m_Headers;
			std::string m_Method;
			std::string m_Path;
		};

		uint64_t UnixNowMs(void*) {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
		}

		NetH4Identity BuildH4Identity(const NetIdentityManifest& manifest) {
			NetH4Identity identity;
			identity.controllerFrameVersion = manifest.controllerFrameVersion;
			identity.controllerFrameEncodedSize = manifest.controllerFrameEncodedSize;
			identity.gameVersion = manifest.gameVersion;
			identity.buildId = manifest.buildId;
			identity.deterministicConfigHash = manifest.deterministicConfigHash;
			identity.moduleManifestHash = manifest.moduleManifestHash;
			identity.sessionRulesHash = manifest.sessionRulesHash;
			identity.sessionIdentityHash = manifest.sessionIdentityHash;
			return identity;
		}

		std::string PlayerNameOrDefault(const NetMatchServiceRequest& request, bool hostSlot) {
			const bool localSlot = request.host == hostSlot;
			if (localSlot && !request.playerName.empty()) {
				return request.playerName;
			}
			return hostSlot ? "Host" : "Client";
		}
	}

	std::vector<NetDirectorySessionRow> BrowseSessionRows(NetDirectoryClient& browse, uint64_t budgetMs, const std::function<bool()>& cancelled) {
		std::vector<NetDirectorySessionRow> rows;
		const uint64_t deadline = SteadyNowMs() + budgetMs;
		while (SteadyNowMs() < deadline && !(cancelled && cancelled())) {
			const uint64_t nowMs = SteadyNowMs();
			browse.PollList(nowMs);
			browse.Update(nowMs);
			if (browse.ListReplies() > 0) {
				rows = browse.Rows();
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		browse.StopBrowsing();
		return rows;
	}

	NetMatchService::NetMatchService() = default;

	NetMatchService::~NetMatchService() {
		Destroy();
	}

	NetMatchService::TransportLink::TransportLink() = default;
	NetMatchService::TransportLink::TransportLink(TransportLink&&) noexcept = default;
	NetMatchService::TransportLink& NetMatchService::TransportLink::operator=(TransportLink&&) noexcept = default;
	NetMatchService::TransportLink::~TransportLink() {
		if (mux) mux->SetPump({});
	}

	bool NetMatchService::Start(const NetMatchServiceRequest& incoming, std::string* error) {
		NetMatchServiceRequest request = incoming;
		Destroy();
		m_CancelRequested.store(false);
		m_ReadyRequested.store(false);
		m_StartRequested.store(false);
		if (request.dedicated && !request.host) {
			if (error) *error = "dedicated service requires the host role";
			return false;
		}
		if (request.port == 0) {
			if (error) *error = "port must be nonzero";
			return false;
		}
		if (!request.host && request.address.empty() && request.sessionId.empty()) {
			if (error) *error = "join address must not be empty";
			return false;
		}
		// Past the refusals: the settings are read once here, where a real host starts, and ride the
		// request to both roster builds, so the worker's copy cannot pick up a later menu edit.
		SeatSavedOptions(request);
		if (request.playerName.size() > NetProtocol::c_MaxDisplayNameBytes) {
			const std::string configError = "display_name exceeds max encoded length";
			if (error) *error = configError;
			SetState(NetMatchServiceState::Failed, "Match roster refused", configError);
			return false;
		}
		// A launch config names its own module; any other request resolves one here, where the loaded
		// modules are known and both roster builds see the answer.
		std::string moduleError;
		if (!request.standardRules && !SeatActivityModule(request, &moduleError)) {
			if (error) *error = moduleError;
			SetState(NetMatchServiceState::Failed, "Match activity refused", moduleError);
			return false;
		}
		std::string sceneError;
		if (!request.standardRules && !SeatHostScene(request, &sceneError)) {
			if (error) *error = sceneError;
			SetState(NetMatchServiceState::Failed, "Match scene refused", sceneError);
			return false;
		}
		NetMatchConfig matchConfig;
		std::string configError;
		if (request.host && (request.persistentWorld || request.activityPreset == "Persistent World")) {
			request.persistentWorld = true;
			request.dedicated = true;
			std::string identityError;
			if (!NetWorldIdentityFile::OpenForBoot(NetWorldIdentityFile::DefaultPath(), m_WorldIdentity, &identityError)) {
				if (error) *error = identityError;
				SetState(NetMatchServiceState::Failed, "World identity refused", identityError);
				return false;
			}
			request.worldId = m_WorldIdentity.worldId;
			request.worldBoot = m_WorldIdentity.boot;
			m_WorldJoin.SetIdentityPath(NetWorldIdentityFile::DefaultPath());
			// The writer thread hashes what it wrote; a multi-megabyte digest is not sim-thread work.
			g_ActivityMan.SetAutosaveDigest([](const std::vector<uint8_t>& bytes) { return DigestWorldJoinBytes(bytes); });
			std::cout << "[net-world] identity " << m_WorldIdentity.worldId << " boot=" << m_WorldIdentity.boot
			          << " round=" << m_WorldIdentity.round << std::endl;
		}
		if (!BuildMatchConfig(request, c_UiSessionId, matchConfig, &configError)) {
			if (error) *error = configError;
			SetState(NetMatchServiceState::Failed, "Match roster refused", configError);
			return false;
		}

		g_TimerMan.SetDeltaTimeSecs(c_DefaultDeltaTimeS);
		NetIdentityManifest manifest;
		NetIdentityBuildOptions identityOptions;
		identityOptions.buildId = "stage2-p2d-local";
		identityOptions.sessionRulesTag = "stage2-p2-session-rules";
		const bool targetingWorld = request.persistentWorld || request.activityPreset == "Persistent World" || matchConfig.persistentWorld;
		m_LastJoinTargetPersistentWorld = targetingWorld;
		NetIdentity::StampOptionsForTarget(identityOptions, targetingWorld);
		std::string buildError;
		if (!NetIdentity::BuildCurrentManifest(manifest, &buildError, identityOptions)) {
			if (error) *error = buildError;
			SetState(NetMatchServiceState::Failed, "Identity build failed", buildError);
			return false;
		}
		CacheDiagnosticIdentity(manifest);

		if (!request.host && NetA7Journal::HasConnectGate() && !WaitForA7ConnectGate(error)) return false;

		// Startup is done and nothing is connected yet, so this is where a held joiner waits.
		if (!request.host && !WaitForJoinTrigger(s_JoinWaitPath, c_JoinWaitBudgetMs, error)) return false;

		m_ActivityPreset = request.activityPreset;
		m_ActivityModule = request.activityModule;
		m_SceneName = request.sceneName;
		m_SceneModule = request.sceneModule;
		SetState(NetMatchServiceState::Starting, request.host ? "Hosting direct-IP match" : "Joining direct-IP match");
		// The directory row advertises the same identity fields the probe registers; only the counts
		// move afterwards. Only a host ever lists itself.
		const std::string directoryListenAddr = NetLanDiscovery::GetPrimaryLocalAddress();
		const bool directoryConfigured = !g_SettingsMan.GetSessionDirectoryUrl().empty();
		const bool iceEnabled = g_SettingsMan.GetNetworkIceEnable() && directoryConfigured &&
		                        (request.host || !request.sessionId.empty());
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_IceEnabled = iceEnabled;
			m_IceBoundSessionId.clear();
			m_IceIdentity.clear();
			m_IceJoinSessionId = request.sessionId;
			m_IceReport.clear();
			m_IceRoute.clear();
			m_DirectorySessionId.clear();
			m_DirectoryToken.clear();
			m_DirectoryRegistered = false;
			m_WorkerDone = false;
			m_IsHost = request.host;
			m_CurrentMatchSummary = {};
			m_SummarySeats.clear();
			m_LocalPeerId = request.host ? 1 : 2;
			m_LocalTeam = request.dedicated ? Activity::NoTeam : (request.host ? 0 : 1);
			m_Dedicated = request.dedicated;
			m_HumanSeats = static_cast<int>(std::count_if(matchConfig.players.begin(), matchConfig.players.end(), [](const auto& slot) { return !slot.cpu; }));
			m_MatchConfig = matchConfig;
			m_InputDelayText.clear();
			m_ResyncOnDesync = request.resyncOnDesync;
			m_PendingResyncLoad.clear();
			m_BeaconGamePort = request.port;
			m_BeaconMaxPlayers = request.dedicated ? static_cast<uint8_t>(std::max(1, static_cast<int>(request.peerCount) - 1)) : request.peerCount;
			m_LocalName = request.playerName.empty() ? (request.host ? "Host" : "Client") : request.playerName;
			m_JoinRefusedByLiveMatch = false;
			if (request.host) {
				m_DirectoryRow.name = m_LocalName;
				m_DirectoryRow.activity = matchConfig.activityPreset;
				m_DirectoryRow.scene = matchConfig.sceneName;
				m_DirectoryRow.mode = NetMatchConfigUtil::ModeName(matchConfig.mode);
				m_DirectoryRow.peerCount = matchConfig.peerCount;
				m_DirectoryRow.seatsFree = std::max<int64_t>(0, static_cast<int64_t>(matchConfig.peerCount) - 1);
				m_DirectoryRow.gameVersion = manifest.gameVersion;
				m_DirectoryRow.buildId = manifest.buildId;
				m_DirectoryRow.networkProtocolVersion = manifest.networkProtocolVersion;
				m_DirectoryRow.lockstepCodecVersion = manifest.deterministicConfig.lockstepCodecVersion;
				m_DirectoryRow.controllerFrameVersion = manifest.controllerFrameVersion;
				m_DirectoryRow.matchConfigHash = NetIdentity::HashHex(NetMatchConfigUtil::HashConfig(matchConfig));
				m_DirectoryRow.sessionIdentityHash = NetIdentity::HashHex(manifest.sessionIdentityHash);
				m_DirectoryRow.moduleManifestHash = NetIdentity::HashHex(manifest.moduleManifestHash);
				m_DirectoryRow.listenPort = request.port;
				m_DirectoryRow.listenAddrs = {directoryListenAddr.empty() ? "127.0.0.1" : directoryListenAddr};
				// The row goes out once, with the intent; a rematch downgrades it (NetIceRowJoinMode).
				m_DirectoryRow.joinMode = NetIceRowJoinMode(iceEnabled, !m_DirectoryRow.listenAddrs.empty(), std::string(), std::string());
				if (matchConfig.persistentWorld) {
					m_DirectoryRow.persistentWorld = true;
					m_DirectoryRow.worldId = matchConfig.worldId;
					m_DirectoryRow.worldBoot = static_cast<int64_t>(matchConfig.worldBoot);
					m_DirectoryRow.resumeSessionId = matchConfig.worldId;
					// The previous boot's row token, so this boot resumes the world's own row.
					m_DirectoryRow.resumeToken = m_WorldIdentity.directoryToken;
				}
			}
			m_DirectoryRetracted = false;
			m_DirectoryHidden = false;
			m_DirectoryRelistPending = false;
		}
		// The mapping request must land before the first directory register: the heartbeat never
		// resends listen_addrs/join_mode, so the row goes out once with its final addresses.
		if (request.host && g_SettingsMan.GetNetworkPortMapEnable()) {
			s_PortMap.Request(request.port, NetPortMap::c_DefaultLeaseS, NetPortMap::ProbeOverrides());
			s_PortMapRequested = true;
		} else {
			s_PortMapRequested = false;
			s_PortMap.Release();
		}
		s_PortMapApplied = false;
		m_EverStarted.store(true);
		m_Worker = std::thread(&NetMatchService::WorkerMain, this, request, std::move(manifest));
		return true;
	}

	bool NetMatchService::ReturnToLobby(std::string* error) {
		JoinWorkerIfDone();
		// A running worker still owns and polls the link.
		if (m_Worker.joinable()) {
			if (error) *error = "the previous match worker is still running";
			return false;
		}
		TransportLink link;
		std::unique_ptr<NetSession> session;
		std::unique_ptr<NetLockstepCoordinator> coordinator;
		std::unique_ptr<NetMatchRunner> runner;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_State != NetMatchServiceState::Completed || !ActiveWireLocked() || !m_Session || !m_Runner) {
				if (error) *error = "no completed match to rematch";
				return false;
			}
			if (m_LeftMatch || m_PendingLobbyOverflow) {
				if (error) *error = m_LeftMatch ? "this match was left" : "the rematch lobby queue overflowed";
				return false;
			}
			if (!m_Session->IsReady()) {
				// Session lost (the other player quit); settle so the UI stops offering a rematch.
				m_State = NetMatchServiceState::Failed;
				m_StatusText = "Rematch unavailable";
				m_ErrorText = m_Session->HasReject() ? m_Session->BuildRejectText() : "the other player left";
				if (error) *error = m_ErrorText;
				return false;
			}
			m_PendingResyncState.reset();
			m_ResyncRetainsLocalState = false;
			m_ResyncSourceRound = 0;
			// The round that just ended is the only thing that knows who left it; the next lobby is
			// formed from the peers it still had. The host derives its own roster from the live session.
			if (!m_IsHost) {
				std::map<uint8_t, uint64_t> leaves;
				std::set<uint8_t> refilled;
				std::optional<NetLockstepSeatSnapshot> seats;
				if (m_Coordinator) {
					leaves = m_Coordinator->GetPeerLeaveFrames();
					for (const auto& leave : leaves) {
						const NetLockstepHoldResolution resolution = m_Coordinator->HeldSeatResolution(leave.first);
						if (resolution == NetLockstepHoldResolution::Reclaimed || resolution == NetLockstepHoldResolution::Substituted) {
							refilled.insert(leave.first);
						}
					}
					seats = m_Coordinator->TakeSeatSnapshot();
					if (!seats) {
						seats = m_SeatPresence.GetSnapshot();
					}
					if (seats && seats->roundId != m_Coordinator->GetRoundId()) {
						seats.reset(); // an earlier round's view
					}
				}
				m_Runner->SetRematchRoster(NetMatchRunner::DeriveRematchSurvivors(m_Runner->GetMatchConfig(), leaves, refilled, seats ? &*seats : nullptr));
			}
			DrainPendingSessionEventsLocked(false);
			AccumulateLockstepTotalsLocked();
			link = TakeTransportLinkLocked();
			session = std::move(m_Session);
			runner = std::move(m_Runner);
			if (m_IsHost) {
				// A rematch restarts from a zeroed sim count.
				runner->SetStartFrame(1);
			}
			m_Coordinator.reset();
			coordinator = std::make_unique<NetLockstepCoordinator>();
			m_WorkerDone = false;
			m_State = NetMatchServiceState::Starting;
			m_StatusText += " - ready up for a rematch";
			m_ErrorText.clear();
			// A bound listener cannot replace a lease lost before the next lobby opens.
			if (!m_IceEnabled || m_IceBoundSessionId.empty()) {
				m_DirectoryRetracted = false;
			}
			m_DirectoryRelistPending = m_DirectoryHidden;
		}
		m_CancelRequested.store(false);
		m_ReadyRequested.store(false);
		m_StartRequested.store(false);
		m_Worker = std::thread(&NetMatchService::WorkerRematchMain, this, std::move(link), session.release(), coordinator.release(), runner.release());
		return true;
	}

	bool NetMatchService::ResyncSnapshotAllowed(const Activity* activity) {
		return activity != nullptr && activity->GetActivityState() != Activity::Over;
	}

	NetRejoinAnswer NetMatchService::ClassifyRejoin(const Activity* activity) {
		return ResyncSnapshotAllowed(activity) ? NetRejoinAnswer::Resync : NetRejoinAnswer::MatchOver;
	}

	void NetMatchService::AnswerMatchOverRejoin(const std::string& result) {
		const std::string text = result.empty() ? "match over" : result;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_RejoinOutcome = "match_over";
			RefuseEndedPeersLocked(text);
		}
		// The main loop owns coordinator teardown.
		Complete(text);
	}

	void NetMatchService::RefuseEndedPeersLocked(const std::string& reason) {
		if (!m_IsHost || !m_Session || !m_Coordinator) return;
		for (const NetSessionPeerInfo& peer : m_Session->GetReadyPeers()) {
			if (!m_Coordinator->UsesTransportPeer(peer.transportPeerId)) {
				m_Session->DisconnectReadyPeer(peer.transportPeerId, NetRejectReason::SessionEnded, reason);
			}
		}
	}

	uint64_t NetLobbyLastStateTransferMs();

	bool NetMatchService::CanResyncLocked(std::string* error) {
		if (m_State != NetMatchServiceState::Running || !ActiveWireLocked() || !m_Session || !m_Runner) {
			if (error) *error = "no live match to resync";
			return false;
		}
		if (!m_Session->IsReady()) {
			m_State = NetMatchServiceState::Failed;
			m_StatusText = "Resync unavailable";
			m_ErrorText = m_Session->HasReject() ? m_Session->BuildRejectText() : "the session was lost";
			if (error) *error = m_ErrorText;
			return false;
		}
		return true;
	}

	NetMatchService::TransportLink NetMatchService::TakeTransportLinkLocked() {
		TransportLink link;
		link.ip = std::move(m_Transport);
		link.mux = std::move(m_Mux);
		link.lobbyEvents = std::move(m_PendingLobbyEvents);
		m_PendingLobbyEvents.clear();
		m_PendingLobbyBytes = 0;
#ifdef CCCP_WITH_GNS
		// The worker's mux pump drives the dispatcher from here, so reports read this copy until it returns.
		if (m_Dispatcher) {
			m_IceReport = m_Dispatcher->BuildReportJson();
		}
		link.dispatcher = std::move(m_Dispatcher);
#endif
		return link;
	}

	void NetMatchService::RestoreTransportLinkLocked(TransportLink link) {
		m_Transport = std::move(link.ip);
		m_Mux = std::move(link.mux);
#ifdef CCCP_WITH_GNS
		m_Dispatcher = std::move(link.dispatcher);
#endif
	}

	bool NetMatchService::ResyncMatch(std::string* error) {
		JoinWorkerIfDone();
		// A running worker still owns and polls the link.
		if (m_Worker.joinable()) {
			if (error) *error = "the previous match worker is still running";
			return false;
		}
		// The host snapshots the live (diverged) match BEFORE the teardown; both sides then reload
		// the identical file, so the divergence is healed by construction.
		std::vector<uint8_t> stateBytes;
		bool isHost = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			// Refused before anything is staged, so the running round keeps its coordinator and pump.
			if (!CanResyncLocked(error)) {
				return false;
			}
			isHost = m_IsHost;
			m_DiagnosticRuntimeError = ScenarioRunner::GetControllerReplayError();
			m_ResyncHealStartMs = SteadyNowMs();
			m_ResyncHealOpen = true;
		}
		const uint64_t a7Resync = NetA7Journal::BeginResync();
		const bool a7Save = NetA7Journal::Enabled() && isHost && FaultInjected("slow_resync_save");
		const uint64_t a7SaveBegin = a7Save ? AdmissionNowMs() : 0;
		if (a7Save) NetA7Journal::Session("save_begin", a7SaveBegin, {{"resync", std::to_string(a7Resync)}}, "NetMatchService::AdmissionNowMs");
		if (isHost && ClassifyRejoin(g_ActivityMan.GetActivity()) == NetRejoinAnswer::MatchOver) {
			AnswerMatchOverRejoin("match over");
			if (error) {
				*error = "match over";
			}
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_ResyncHealOpen = false;
			return false;
		}
		uint64_t dropFrame = 0;
		if (isHost) {
			// Snapshot callbacks stay on the game thread while waiting peers keep hearing from us.
			std::jthread keepalive([this](std::stop_token stop) {
				while (!stop.stop_requested()) {
					{
						std::lock_guard<std::mutex> lock(m_Mutex);
						if (m_Session) m_Session->TickKeepalive(AdmissionNowMs());
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
				}
			});
			// The healed round resumes at the first frame the sim has not applied, and the world may only
			// be saved where no tick is in flight.
			const uint64_t resumeFrame = ScenarioRunner::GetLockstepResumeFrame();
			// The tick the live world actually stands on; equal to the label only where no tick is in flight.
			const uint64_t simTickAtSave = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
			if (!ScenarioRunner::ResolveResyncDropFrame(resumeFrame, simTickAtSave, dropFrame, error)) {
				return false;
			}
			if (!g_ActivityMan.SaveCurrentGame(ResyncSaveName(), ActivityMan::SaveCompression::Small) || !g_ActivityMan.WaitForSaveGameTask()) {
				if (error) *error = "resync snapshot save failed";
				return false;
			}
			const uint64_t savedTick = dropFrame > 0 ? dropFrame - 1 : 0;
			m_ResyncSavedTick.store(savedTick);
			m_ResyncBoundaryTick.store(simTickAtSave);
			std::cout << "[net-match] resync snapshot at tick " << savedTick << " (completed " << simTickAtSave << ")" << std::endl;
			if (FaultInjected("slow_resync_save")) {
				// Keep the snapshot frozen across a save longer than the receive timeout.
				std::this_thread::sleep_for(std::chrono::seconds(7));
			}
			if (a7Save) {
				const uint64_t ended = AdmissionNowMs();
				NetA7Journal::Session("save_end", ended, {{"resync", std::to_string(a7Resync)}, {"fault", "slow_resync_save"}, {"elapsed_ms", ended - a7SaveBegin}}, "NetMatchService::AdmissionNowMs");
			}
			const std::string savePath = g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/" + ResyncSaveName() + ".ccsave";
			std::ifstream in(savePath, std::ios::binary);
			if (!in) {
				if (error) *error = "resync snapshot is unreadable: " + savePath;
				return false;
			}
			stateBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			if (stateBytes.empty()) {
				if (error) *error = "resync snapshot is empty";
				return false;
			}
			NetResyncState state;
			std::vector<uint8_t> envelope;
			if (!ScenarioRunner::CaptureNetResyncState(dropFrame > 0 ? dropFrame - 1 : 0, state, error) || !NetResyncCodec::Encode(state, stateBytes, envelope, error)) return false;
			const uint64_t archiveBytes = stateBytes.size();
			const uint64_t envelopeBytes = envelope.size();
			const uint64_t saveMs = static_cast<uint64_t>(std::max(0LL, g_ActivityMan.LastSaveMainMs()));
			const uint64_t zipMs = static_cast<uint64_t>(std::max(0LL, g_ActivityMan.LastSaveZipMs()));
			std::cout << "[net-match] resync snapshot: archive=" << archiveBytes << " envelope=" << envelopeBytes
			          << " save_ms=" << saveMs << " zip_ms=" << zipMs << std::endl;
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_LastResync.archiveBytes = archiveBytes;
				m_LastResync.envelopeBytes = envelopeBytes;
				m_LastResync.saveMs = saveMs;
				m_LastResync.happened = true;
			}
			stateBytes = std::move(envelope);
		}
		TransportLink link;
		std::unique_ptr<NetSession> session;
		std::unique_ptr<NetLockstepCoordinator> coordinator;
		std::unique_ptr<NetMatchRunner> runner;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			// The host's snapshot ran unlocked while keepalives ticked the session.
			if (!CanResyncLocked(error)) {
				m_ResyncHealOpen = false;
				return false;
			}
			m_ResyncSourceRound = ScenarioRunner::GetLockstepRoundId();
			m_ResyncRetainsLocalState = true;
			ScenarioRunner::SetLockstepCoordinator(nullptr);
			ScenarioRunner::SetSessionPump(nullptr);
			// The round the resync destroys may be holding a fenced peer's disconnect it took off the
			// transport; the session is the only thing here that outlives the coordinator.
			DrainPendingSessionEventsLocked(true);
			AccumulateLockstepTotalsLocked();
			link = TakeTransportLinkLocked();
			session = std::move(m_Session);
			runner = std::move(m_Runner);
			if (isHost) {
				runner->SetStartFrame(ScenarioRunner::ResyncResumeStartFrame(dropFrame));
			}
			m_Coordinator.reset();
			coordinator = std::make_unique<NetLockstepCoordinator>();
			m_WorkerDone = false;
			m_State = NetMatchServiceState::Starting;
			m_StatusText = "Resyncing the match";
			m_ErrorText.clear();
			m_PendingResyncLoad.clear();
		}
		m_CancelRequested.store(false);
		m_ReadyRequested.store(true);
		m_StartRequested.store(isHost);
		m_Worker = std::thread(&NetMatchService::WorkerResyncMain, this, std::move(link), session.release(), coordinator.release(), runner.release(), std::move(stateBytes));
		return true;
	}

	void NetMatchService::NoteResyncRelaunched() {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_ResyncHealOpen) {
			return;
		}
		const uint64_t now = SteadyNowMs();
		m_LastResync.healMs = now >= m_ResyncHealStartMs ? now - m_ResyncHealStartMs : 0;
		m_ResyncHealOpen = false;
		m_LastResync.happened = true;
	}

	void NetMatchService::WorkerResyncMain(TransportLink link, NetSession* sessionRaw, NetLockstepCoordinator* coordinatorRaw, NetMatchRunner* runnerRaw, std::vector<uint8_t> stateBytes) {
		std::unique_ptr<NetSession> session(sessionRaw);
		std::unique_ptr<NetLockstepCoordinator> coordinator(coordinatorRaw);
		std::unique_ptr<NetMatchRunner> runner(runnerRaw);
		std::string error;
		const bool started = runner->StartNextMatch(*link.Wire(), *session, *coordinator, &error, stateBytes, std::move(link.lobbyEvents));
		const uint64_t transferMs = NetLobbyLastStateTransferMs();
		std::string pendingLoad;
		NetResyncState resyncState;
		size_t receivedArchive = 0;
		size_t receivedEnvelope = 0;
		if (started) {
			std::vector<uint8_t> receivedState = runner->TakeReceivedState();
			if (receivedState.empty()) receivedState = std::move(stateBytes);
			receivedEnvelope = receivedState.size();
			(void)PrepareReceivedResync(receivedState, *coordinator, pendingLoad, resyncState, &error, &receivedArchive);
		}
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_LastResync.transferMs = transferMs;
			if (!m_IsHost && receivedEnvelope != 0) {
				m_LastResync.archiveBytes = receivedArchive;
				m_LastResync.envelopeBytes = receivedEnvelope;
				m_LastResync.happened = true;
			}
			RestoreTransportLinkLocked(std::move(link));
			m_Session = std::move(session);
			m_Coordinator = std::move(coordinator);
			m_Runner = std::move(runner);
			if (started && !pendingLoad.empty()) {
				m_PendingResyncLoad = pendingLoad;
				m_PendingResyncState = std::move(resyncState);
				m_State = NetMatchServiceState::ReadyToLaunch;
				m_StatusText = "Resynced; relaunching match";
				m_ErrorText.clear();
			} else {
				m_State = NetMatchServiceState::Failed;
				m_StatusText = "Resync failed";
				m_ErrorText = error;
			}
			m_WorkerDone = true;
		}
	}

	bool NetMatchService::PrepareReceivedResync(const std::vector<uint8_t>& bytes, const NetLockstepCoordinator& coordinator, std::string& pendingLoad, NetResyncState& state, std::string* error, size_t* archiveBytes) {
		std::vector<uint8_t> archive;
		if (!NetResyncCodec::Decode(bytes, coordinator.GetConfig().sessionId, coordinator.GetConfig().startFrame, state, archive, error)) return false;
		if (archiveBytes) *archiveBytes = archive.size();
		if ((m_ResyncSourceRound != 0 && state.sourceRound != m_ResyncSourceRound) || state.sourceRound == coordinator.GetRoundId()) {
			if (error) *error = "resync snapshot round does not match the ended match";
			return false;
		}
		const auto name = ResyncSaveName() + "_recv";
		const auto path = g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/" + name + ".ccsave";
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size()));
		out.close();
		if (!out.good()) { if (error) *error = "could not write the received resync snapshot"; return false; }
		pendingLoad = name;
		return true;
	}

	std::string NetMatchService::TakePendingResyncLoad() {
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::string pending = std::move(m_PendingResyncLoad);
		m_PendingResyncLoad.clear();
		return pending;
	}

	bool NetMatchService::HasPendingResyncLoad() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return !m_PendingResyncLoad.empty();
	}

	bool NetMatchService::StageResyncedMatchLaunch(std::string* error) {
		if (m_WorldCatchUp.active) {
			const std::string pendingLoad = TakePendingResyncLoad();
			if (pendingLoad.empty()) {
				if (error) *error = "no world join snapshot to load";
				return false;
			}
			if (!g_ActivityMan.LoadGameToRestart(pendingLoad)) {
				if (error) *error = "world join snapshot load failed: " + pendingLoad;
				return false;
			}
			if (!ScenarioRunner::InstallWorldCatchUp(m_WorldCatchUp.snapshotTick, m_WorldCatchUp.tail, error)) {
				return false;
			}
			return true;
		}
		const std::string pendingLoad = TakePendingResyncLoad();
		if (pendingLoad.empty() || !m_PendingResyncState) {
			if (error) *error = "no resync snapshot to load";
			return false;
		}
		const auto state = std::make_shared<NetResyncState>(*m_PendingResyncState);
		const uint8_t localPeer = GetLocalPeerId();
		const bool retainLocal = m_ResyncRetainsLocalState;
		// A seatless dedicated host has no binding in any snapshot and no local state to keep.
		const bool dedicated = m_Dedicated;
		std::optional<NetResyncPlayerBindings> newestBinding;
		if (const auto found = state->playerBindings.find(localPeer); found != state->playerBindings.end()) newestBinding = found->second;
		for (const auto& pending: state->pendingPlayerBindings) if (pending.command.senderPeerId == localPeer && (!newestBinding || pending.frame > newestBinding->frame)) {
			newestBinding = NetResyncPlayerBindings{pending.frame, std::get<NetGamePlayerBindings>(pending.command.payload)};
		}
		if (!retainLocal && !newestBinding && !dedicated) {
			if (error) *error = "the resync snapshot has no player bindings for this peer";
			return false;
		}
		if (!g_ActivityMan.LoadGameToRestart(pendingLoad)) {
			if (error) *error = "resync snapshot load failed: " + pendingLoad;
			return false;
		}
		g_ActivityMan.NoteLockstepRelaunch();
		const char* keepResyncSaves = std::getenv("CC_KEEP_RESYNC_SAVES");
		if (keepResyncSaves && keepResyncSaves[0] && keepResyncSaves[0] != '0') {
			std::cout << "[net-match] keeping resync save: " << pendingLoad << std::endl;
		} else {
			g_ActivityMan.RemoveSavedGame(pendingLoad);
		}
		std::cout << "[net-match] launching from the received snapshot: " << pendingLoad << std::endl;
		struct LocalState { Activity::NetLocalPlayerState activity; std::string input, gui, frame; };
		const auto local = std::make_shared<LocalState>();
		const bool keepLocalPlayer = retainLocal && !dedicated;
		if (!g_ActivityMan.SetPendingCheckpointCallbacks([local, keepLocalPlayer] {
			local->input = g_UInputMan.SaveCheckpoint();
			local->gui = GUIInput::SaveSharedCheckpoint();
			local->frame = g_FrameMan.SaveNetLocalState();
			return !keepLocalPlayer || (g_ActivityMan.GetActivity() && g_ActivityMan.GetActivity()->CaptureNetLocalPlayerState(local->activity));
		}, [local, state, keepLocalPlayer, dedicated, newestBinding](Activity& activity) {
			if (static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) != state->savedTick ||
			    !g_UInputMan.LoadCheckpoint(local->input, true) || !GUIInput::LoadSharedCheckpoint(local->gui, true) || !g_FrameMan.LoadNetLocalState(local->frame, true)) return false;
			const NetGamePlayerBindings seatless{};
			if (!(keepLocalPlayer ? activity.RestoreNetLocalPlayerState(local->activity) : activity.ApplyNetPlayerBindings(dedicated ? seatless : newestBinding->bindings))) return false;
			if (!g_UInputMan.LoadCheckpoint(local->input) || !GUIInput::LoadSharedCheckpoint(local->gui) || !g_FrameMan.LoadNetLocalState(local->frame)) return false;
			return ScenarioRunner::RestoreNetResyncState(*state);
		})) { if (error) *error = "could not stage resync local state restoration"; return false; }
		ScenarioRunner::ApplyDeterministicConfig();
		return true;
	}

	// Same shape as WorkerMain: the objects live as worker locals while the lobby round runs, so
	// report/snapshot readers never race a mid-mutation runner; they move back in when it settles.
	void NetMatchService::WorkerRematchMain(TransportLink link, NetSession* sessionRaw, NetLockstepCoordinator* coordinatorRaw, NetMatchRunner* runnerRaw) {
		std::unique_ptr<NetSession> session(sessionRaw);
		std::unique_ptr<NetLockstepCoordinator> coordinator(coordinatorRaw);
		std::unique_ptr<NetMatchRunner> runner(runnerRaw);
		std::string error;
		const bool started = runner->StartNextMatch(*link.Wire(), *session, *coordinator, &error, {}, std::move(link.lobbyEvents));
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (started) {
				// A rematch may be played on fewer seats than the last round, so everything keyed on the
				// roster is re-read from the config the round actually starts on.
				const NetMatchConfig& roster = runner->GetMatchConfig();
				const uint8_t localLockstepId = static_cast<uint8_t>(session->GetLocalPeerId() + 1);
				m_LocalPeerId = localLockstepId;
				for (const NetMatchPlayerSlot& slot : roster.players) {
					if (slot.peerId == localLockstepId) {
						m_LocalTeam = slot.team;
						break;
					}
				}
				m_HumanSeats = static_cast<int>(std::count_if(roster.players.begin(), roster.players.end(), [](const auto& slot) { return !slot.cpu; }));
				if (m_IsHost) {
					// The beacon and the directory row take their counts from these at the next pump.
					m_BeaconMaxPlayers = static_cast<uint8_t>(std::max(1, m_HumanSeats));
					m_DirectoryRow.matchConfigHash = NetIdentity::HashHex(runner->GetMatchConfigHash());
				}
			}
			RestoreTransportLinkLocked(std::move(link));
			m_Session = std::move(session);
			m_Coordinator = std::move(coordinator);
			m_Runner = std::move(runner);
			if (started) {
				m_State = NetMatchServiceState::ReadyToLaunch;
				m_StatusText = "Ready to launch match";
				m_ErrorText.clear();
			} else {
				m_State = NetMatchServiceState::Failed;
				m_StatusText = "Rematch setup failed";
				m_ErrorText = error;
			}
			m_WorkerDone = true;
		}
	}

	void NetMatchService::ResetRosterTransitionHistory() {
		m_RosterTransitions.clear();
		m_RosterTransitionsDropped = 0;
		m_LastRosterPair.clear();
	}

	void NetMatchService::RecordRosterTransitions(uint64_t observedAtMs) {
		UpdateSummarySeatsLocked();
		const uint64_t appliedFrame = ScenarioRunner::GetLockstepAppliedFrame();
		for (const NetLobbyMember& member: m_LobbySnapshot.members) {
			const std::string state = NetSeatPresence::StateName(m_SeatPresence.StateOf(member.peerId));
			const std::string line = m_SeatPresence.Line(member.peerId, member.displayName);
			std::pair<std::string, std::string>& last = m_LastRosterPair[member.peerId];
			if (last.first == state && last.second == line) {
				continue;
			}
			const std::string previous = last.first;
			const std::string previousLine = last.second;
			last = {state, line};
			if (!member.cpu) {
				const auto seat = m_SeatPresence.GetSeats().find(member.peerId);
				const std::string name = seat != m_SeatPresence.GetSeats().end() && !seat->second.holderName.empty() ? seat->second.holderName : member.displayName;
				const std::string who = name.empty() ? "Player " + std::to_string(member.peerId) : name;
				const bool wasAway = previous == "Disconnected" || previous == "Reconnecting" || previous == "Left";
				if (previous.empty() && state == "Present" && member.peerId != m_LocalPeerId) {
					ScenarioRunner::PushNetUiToast("player_joined", who + " joined");
				} else if (state == "Disconnected" && !previous.empty() && !wasAway) {
					ScenarioRunner::PushNetUiToast("player_dropped", who + " dropped");
				} else if (state == "Left" && !previous.empty() && previous != "Left") {
					ScenarioRunner::PushNetUiToast("player_left", who + " left");
				} else if (state == "Present" && wasAway) {
					ScenarioRunner::PushNetUiToast("player_rejoined", who + " rejoined");
				} else if (state == "Substituted" && (previous != state || previousLine != line)) {
					ScenarioRunner::PushNetUiToast("player_substituted", who + " joined as substitute");
				}
			}
			if (m_RosterTransitions.size() >= 256) {
				++m_RosterTransitionsDropped;
				continue;
			}
			m_RosterTransitions.push_back({member.peerId, state, line, appliedFrame, observedAtMs});
		}
	}

	void NetMatchService::EndAdmissionSession() {
		if (m_AdmissionAttached && m_IsHost && m_Session) {
			// P22: sent from the same call that clears the registry, so a client's record is provably
			// dead exactly here - not on a transport disconnect, a timeout or a match Complete.
			m_Session->EndHostedSession("the host ended the session");
		}
		m_SeatAuth.EndSession();
		m_ReconnectHost.EndHostedSession();
		m_ReconnectHost.TakeOutbound();
		m_SeatStatuses.clear();
		m_SeatPresence.Clear();
		ResetRosterTransitionHistory();
		m_ModerationSeats.clear();
		// An action queued for a setup worker that is gone belongs to no session and is dropped here.
		m_PendingModeration.clear();
		m_AdmissionAttached = false;
	}

	void NetMatchService::RunCleanLeave() {
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_LeaveExchangeRun) {
				return;
			}
			m_LeaveExchangeRun = true;
			// Without a live link the recovery ticket must survive.
			if (!m_AdmissionAttached || m_IsHost || !m_Session || !m_Session->IsReady() || !m_TicketStore.HasRecord()) {
				return;
			}
			std::string error;
			if (!m_ReconnectClient.BeginLeave(AdmissionNowMs(), &error)) {
				return;
			}
		}
		for (;;) {
			{
				// The lock is dropped for the wait, so the game thread's own reads never stall on it.
				std::lock_guard<std::mutex> lock(m_Mutex);
				if (!m_Session) {
					break;
				}
				const uint64_t nowMs = AdmissionNowMs();
				m_Session->Tick(nowMs);
				// The client deadline must run even after the transport closes.
				m_ReconnectClient.Tick(nowMs);
				if (m_ReconnectClient.GetState() != NetH4ClientState::Leaving || m_ReconnectClient.WantsLinkClosed()) {
					break;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::cout << "[net-reconnect] leave: " << NetReconnectClientStateName(m_ReconnectClient.GetState())
		          << (m_TicketStore.HasRecord() ? " (ticket kept)" : " (ticket cleared)") << std::endl;
	}

	void NetMatchService::LeaveWorkerMain(std::string result) {
		RunCleanLeave();
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_Coordinator) {
			m_Coordinator->Leave(result);
		}
		m_WorkerDone = true;
	}

	void NetMatchService::WaitForPendingWork() {
		if (m_Worker.joinable()) {
			m_Worker.join();
		}
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_WorkerDone = false;
	}

	void NetMatchService::Destroy() {
		m_CancelRequested.store(true);
		if (m_Worker.joinable()) {
			m_Worker.join();
		}
		RunCleanLeave();
		m_LanDiscovery.Stop();
		m_Directory.Shutdown(); // the DELETE goes out before the row would expire
		s_PortMap.Release();    // the router mapping goes out with the listing
		ScenarioRunner::SetLockstepCoordinator(nullptr);
		ScenarioRunner::SetSessionPump(nullptr);
		std::unique_ptr<NetMatchRunner> runner;
		std::unique_ptr<NetLockstepCoordinator> coordinator;
		std::unique_ptr<NetSession> session;
		std::unique_ptr<GnsTransport> transport;
		std::unique_ptr<NetMuxTransport> mux;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_Mux) m_Mux->SetPump({});
#ifdef CCCP_WITH_GNS
			if (m_Dispatcher) {
				m_IceReport = m_Dispatcher->BuildReportJson();
				m_Dispatcher->Stop();
				m_Dispatcher.reset();
			}
#endif
			mux = std::move(m_Mux);
			DrainPendingSessionEventsLocked(false);
			m_IceEnabled = false;
			m_IceBoundSessionId.clear();
			m_IceIdentity.clear();
			m_IceJoinSessionId.clear();
			m_IceRoute.clear();
			m_DirectorySessionId.clear();
			m_DirectoryToken.clear();
			m_DirectoryRegistered = false;
			m_DirectoryHidden = false;
			m_DirectoryRelistPending = false;
			AccumulateLockstepTotalsLocked();
			runner = std::move(m_Runner);
			coordinator = std::move(m_Coordinator);
			session = std::move(m_Session);
			transport = std::move(m_Transport);
			m_ChatSession = nullptr;
			m_WorkerDone = false;
			m_IsHost = false;
			m_LocalPeerId = 0;
			m_LocalTeam = -1;
			m_Dedicated = false;
			m_HumanSeats = 0;
			m_MatchConfig = {};
			m_ResyncOnDesync = false;
			m_PendingResyncLoad.clear();
			m_PendingResyncState.reset();
			m_ResyncRetainsLocalState = false;
			m_ResyncSourceRound = 0;
			m_LastRoundId = 0;
			m_PendingToasts.clear();
			m_LocalName.clear();
			m_ActivityPreset.clear();
			m_SceneName.clear();
			m_SceneModule.clear();
			m_State = NetMatchServiceState::Idle;
			m_StatusText = "Idle";
			m_ErrorText.clear();
			m_LobbySnapshot = {};
			m_InputDelayText.clear();
			m_LeaveExchangeRun = false;
			m_MatchWasRunning = false;
			m_LeftMatch = false;
			m_CompletedLobbySinceMs = 0;
			m_PendingLobbyEvents.clear();
			m_PendingLobbyBytes = 0;
			m_PendingLobbyOverflow = false;
			m_EndedLockstepPackets = 0;
			m_JoinRefusedByLiveMatch = false;
			EndAdmissionSession();
		}
		runner.reset();
		coordinator.reset();
		session.reset();
		mux.reset();
		transport.reset();
	}

	void NetMatchService::ReportRuntimeError(const std::string& error) {
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_DiagnosticRuntimeError = error;
		}
		m_CancelRequested.store(true);
		if (m_Worker.joinable()) {
			m_Worker.join();
		}
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			CaptureMatchSummaryLocked(error);
		}
		RetractDirectoryListing();
		ScenarioRunner::SetLockstepCoordinator(nullptr);
		ScenarioRunner::SetSessionPump(nullptr);
		std::unique_ptr<NetMatchRunner> runner;
		std::unique_ptr<NetLockstepCoordinator> coordinator;
		std::unique_ptr<NetSession> session;
		std::unique_ptr<GnsTransport> transport;
		std::unique_ptr<NetMuxTransport> mux;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_Mux) m_Mux->SetPump({});
#ifdef CCCP_WITH_GNS
			if (m_Dispatcher) {
				m_IceReport = m_Dispatcher->BuildReportJson();
				m_Dispatcher->Stop();
				m_Dispatcher.reset();
			}
#endif
			mux = std::move(m_Mux);
			DrainPendingSessionEventsLocked(false);
			AccumulateLockstepTotalsLocked();
			if (m_CapturedRunnerReport.empty() && m_Runner && m_Session && m_Coordinator) {
				m_CapturedRunnerReport = m_Runner->BuildReportJson(*m_Session, *m_Coordinator);
			}
			runner = std::move(m_Runner);
			coordinator = std::move(m_Coordinator);
			session = std::move(m_Session);
			transport = std::move(m_Transport);
			m_ChatSession = nullptr;
			m_WorkerDone = false;
			m_IsHost = false;
			m_LocalPeerId = 0;
			m_LocalTeam = -1;
			m_ResyncOnDesync = false;
			m_PendingResyncLoad.clear();
			m_LocalName.clear();
			m_State = NetMatchServiceState::Failed;
			m_PendingLobbyEvents.clear();
			m_PendingLobbyBytes = 0;
			m_StatusText = "Match stopped";
			m_ErrorText = error;
			m_LobbySnapshot = {};
			m_InputDelayText.clear();
			EndAdmissionSession();
		}
		runner.reset();
		coordinator.reset();
		session.reset();
		mux.reset();
		transport.reset();
	}

	void NetMatchService::RetractDirectoryListing() {
		m_DirectoryHidden = false;
		m_DirectoryRelistPending = false;
		m_DirectoryRetracted = true;
		m_Directory.Retract();
		// Kick the delete now: callers that quit right after never Update() again, and Destroy's
		// Shutdown() would otherwise have to find the slot itself.
		m_Directory.Update(SteadyNowMs());
	}

	bool NetMatchService::ShouldKeepIceDirectoryLease() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_IsHost && m_IceEnabled && !m_DirectoryRetracted && !m_IceBoundSessionId.empty() &&
		       m_Directory.GetState() == NetDirectoryClient::State::Registered && m_Directory.GetSessionId() == m_IceBoundSessionId;
	}

	void NetMatchService::HideDirectoryListing() {
		NetDirectoryRegisterRequest advertised;
		bool running;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			advertised = m_DirectoryRow;
			running = m_State == NetMatchServiceState::Running;
		}
		m_DirectoryHidden = true;
		m_DirectoryRelistPending = false;
		m_Directory.Advertise(advertised, running, false);
		m_Directory.Update(SteadyNowMs());
	}

	void NetMatchService::SettleKeptDirectoryLease() {
		if (!m_DirectoryHidden || m_Directory.GetState() == NetDirectoryClient::State::Deleting) {
			return;
		}
		if (!ShouldKeepIceDirectoryLease()) {
			RetractDirectoryListing();
		} else if (m_DirectoryRelistPending && m_Directory.GetConfirmedListed() == false) {
			m_DirectoryHidden = false;
			m_DirectoryRelistPending = false;
		}
	}

	void NetMatchService::Complete(const std::string& reason) {
		std::string displayReason = reason;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			CaptureMatchSummaryLocked(reason);
			if (m_LastMatchSummary) displayReason = m_LastMatchSummary->result;
		}
		// The recording gets its end marker at the match's end, not at process exit.
		ScenarioRunner::CloseLockstepReplayRecord();
		if (ShouldKeepIceDirectoryLease()) {
			HideDirectoryListing();
		} else {
			RetractDirectoryListing();
		}
		std::lock_guard<std::mutex> lock(m_Mutex);
		DrainPendingSessionEventsLocked(false);
		if (m_Coordinator) {
			m_Coordinator->Complete(reason);
		}
		if (m_State == NetMatchServiceState::Running) {
			m_StatusText = displayReason.empty() ? "Match complete" : displayReason;
			m_ErrorText.clear();
		}
	}

	// Terminal clean end; the session objects stay alive for the next Start or quit.
	void NetMatchService::FinishMatch(const std::string& result) {
		std::string displayResult = result;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			CaptureMatchSummaryLocked(result);
			if (m_LastMatchSummary) displayResult = m_LastMatchSummary->result;
			if (m_IsHost) m_ReconnectHost.SetMatchEnded();
			DrainPendingSessionEventsLocked(false);
		}
		ScenarioRunner::SetLockstepCoordinator(nullptr);
		ScenarioRunner::ResetRetiredChecksumCounters();
		ScenarioRunner::SetSessionPump(nullptr);
		if (ShouldKeepIceDirectoryLease()) {
			HideDirectoryListing();
		} else {
			RetractDirectoryListing();
		}
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_Coordinator) {
			m_Coordinator->Complete(result.empty() ? "match over" : result);
		}
		if (m_State == NetMatchServiceState::Running) {
			m_State = NetMatchServiceState::Completed;
			// The rematch lobby this end opens starts waiting for the other peers here.
			m_CompletedLobbySinceMs = SteadyNowMs();
			m_StatusText = displayResult.empty() ? "Match complete" : displayResult;
			m_ErrorText.clear();
		}
	}

	void NetMatchService::LeaveMatch(const std::string& result) {
		std::string displayResult = result;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			CaptureMatchSummaryLocked(result);
			if (m_LastMatchSummary) displayResult = m_LastMatchSummary->result;
			if (m_IsHost) m_ReconnectHost.SetMatchEnded();
			m_LeftMatch = true;
			DrainPendingSessionEventsLocked(false);
		}
		ScenarioRunner::SetLockstepCoordinator(nullptr);
		ScenarioRunner::SetSessionPump(nullptr);
		RetractDirectoryListing();
		const std::string reason = result.empty() ? std::string("player left") : result;
		bool exchangeOwed = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			// §7 runs while the link is still up, so the round is told only once the ack has settled or
			// the P21 budget has run out. Without a ticket to answer for there is nothing to wait on.
			exchangeOwed = !m_LeaveExchangeRun && m_AdmissionAttached && !m_IsHost && m_Session &&
			               m_Session->IsReady() && m_TicketStore.HasRecord();
			if (!exchangeOwed && m_Coordinator) {
				m_Coordinator->Leave(reason);
			}
			if (m_State == NetMatchServiceState::Running) {
				m_State = NetMatchServiceState::Completed;
				m_StatusText = displayResult.empty() ? "Left the match" : displayResult;
				m_ErrorText.clear();
			}
		}
		if (!exchangeOwed) {
			return;
		}
		// The wait belongs to the worker: a sim thread must never sleep on a network answer.
		WaitForPendingWork();
		m_Worker = std::thread(&NetMatchService::LeaveWorkerMain, this, reason);
	}

	void NetMatchService::Update() {
		// Both the multiplayer screen and the menu loop's recovery pump call this; one pump per
		// millisecond is one pump per frame at any frame rate a menu runs at.
		const uint64_t nowMs = SteadyNowMs();
		if (m_LastUpdateMs == nowMs) {
			return;
		}
		m_LastUpdateMs = nowMs;
		PushPendingToasts();
		JoinWorkerIfDone();
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			PumpCompletedSessionLocked();
		}
		SettleKeptDirectoryLease();
		// A hosting lobby advertises itself on the LAN until the match launches.
		bool beaconWanted = false;
		bool directoryWanted = false;
		bool directoryRunning = false;
		bool directoryListed = true;
		int64_t directorySeatsFree = 0;
		NetLobbySnapshot snapshot;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			beaconWanted = m_IsHost && m_State == NetMatchServiceState::Starting;
			if (beaconWanted) {
				snapshot = m_LobbySnapshot;
			}
			directoryWanted = m_IsHost && !m_DirectoryRetracted &&
			                  (m_State == NetMatchServiceState::Starting || m_State == NetMatchServiceState::ReadyToLaunch ||
			                   m_State == NetMatchServiceState::Running || (m_DirectoryHidden && m_State == NetMatchServiceState::Completed));
			directoryRunning = m_State == NetMatchServiceState::Running;
			directoryListed = !m_DirectoryHidden;
			if (directoryWanted) {
				if (directoryRunning && !m_SeatStatuses.empty()) {
					// The admission table says which seats a late joiner could still take: the host's
					// own seat and the CPU slot never count, a committed or closed one is taken.
					for (const NetH4SeatStatus& seat : m_SeatStatuses) {
						if (seat.lockstepPeerId != 0 && seat.lockstepPeerId != m_LocalPeerId && !seat.committed && !seat.closed) {
							++directorySeatsFree;
						}
					}
				} else if (!m_LobbySnapshot.members.empty()) {
					// The roster lists every configured slot, so an open seat is a non-CPU slot no
					// peer has connected into yet.
					for (const NetLobbyMember& member : m_LobbySnapshot.members) {
						if (!member.cpu && !member.connected) {
							++directorySeatsFree;
						}
					}
				} else {
					directorySeatsFree = std::max<int64_t>(0, static_cast<int64_t>(m_BeaconMaxPlayers) - 1);
				}
			}
		}
		if (beaconWanted) {
			if (!m_HostLobbyBeaconed) {
				const std::string address = NetLanDiscovery::GetPrimaryLocalAddress();
				{
					std::lock_guard<std::mutex> lock(m_Mutex);
					m_ReconnectHost.SetHostAddress(address);
					m_HostLobbyBeaconed = true;
				}
			}
			std::string ignored;
			NetLanCompatIdentity beaconCompat;
			beaconCompat.networkProtocolVersion = m_DirectoryRow.networkProtocolVersion;
			beaconCompat.lockstepCodecVersion = m_DirectoryRow.lockstepCodecVersion;
			beaconCompat.controllerFrameVersion = m_DirectoryRow.controllerFrameVersion;
			beaconCompat.sessionIdentityHash = m_DirectoryRow.sessionIdentityHash;
			beaconCompat.moduleManifestHash = m_DirectoryRow.moduleManifestHash;
			(void)m_LanDiscovery.StartBeacon(m_BeaconGamePort,
			                                 m_LocalName.empty() ? "Host" : m_LocalName,
			                                 snapshot.activityPreset.empty() ? m_ActivityPreset : snapshot.activityPreset,
			                                 snapshot.modeName,
			                                 static_cast<uint8_t>(std::max<size_t>(snapshot.members.size(), 1)),
			                                 m_BeaconMaxPlayers, &beaconCompat, &ignored);
			m_LanDiscovery.Tick(nowMs);
		} else {
			m_HostLobbyBeaconed = false;
			if (m_LanDiscovery.IsBeaconing()) {
				m_LanDiscovery.Stop();
			}
		}
		// The directory row lives beside the beacon but outlives it: it comes up with the lobby and
		// keeps beating while the match runs so a late joiner (or a dedicated host's row) resolves.
		// Configure runs unconditionally so an empty URL lands the client in Disabled, which is what
		// the report's service.directory.state must show.
		if (s_PortMapRequested) {
			s_PortMap.Update(nowMs);
		}
		if (s_PortMapRequested && s_PortMap.Mapped()) {
			// The public endpoint leads; the LAN address stays as the fallback join path.
			const std::string external = s_PortMap.GetResult().externalIp;
			if (!external.empty()) {
				std::lock_guard<std::mutex> lock(m_Mutex);
				std::vector<std::string> addrs = m_DirectoryRow.listenAddrs;
				if (addrs.empty() || addrs.front() != external) {
					addrs.erase(std::remove(addrs.begin(), addrs.end(), external), addrs.end());
					addrs.insert(addrs.begin(), external);
					m_DirectoryRow.listenAddrs = addrs;
					m_DirectoryRow.joinMode = "either";
					m_Directory.NoteListenAddrs(addrs);
				}
				s_PortMapApplied = true;
			}
		}
		const std::string& directoryUrl = g_SettingsMan.GetSessionDirectoryUrl();
		// The install key is minted on the first directory use, so only a listing host asks for it.
		const std::string directoryKey = (directoryWanted && !directoryUrl.empty()) ? g_SettingsMan.GetOrCreateSessionDirectoryInstallKey() : g_SettingsMan.GetSessionDirectoryInstallKey();
		const std::string directoryCertPin = g_SettingsMan.GetSessionDirectoryCertSha256();
		if (s_PortMapRequested) {
			m_Directory.SetTransportFactory([directoryUrl, directoryKey, directoryCertPin]() {
				// The factory sees the raw settings value; Configure's own copy gets this normalization.
				std::string baseUrl = directoryUrl;
				while (!baseUrl.empty() && baseUrl.back() == '/') {
					baseUrl.pop_back();
				}
				if (!baseUrl.empty() && baseUrl.rfind("https://", 0) != 0) {
					baseUrl = "https://" + baseUrl;
				}
				return std::make_unique<ObservedIpTransport>(baseUrl, directoryKey, directoryCertPin);
			});
		}
		m_Directory.Configure(directoryUrl, directoryKey, directoryCertPin);
		// While the mapper is still working the register must wait: the row is sent exactly once.
		if (directoryWanted && (!s_PortMapRequested || s_PortMap.Done())) {
			NetDirectoryRegisterRequest advertised;
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_DirectoryRow.peerCount = m_BeaconMaxPlayers;
				m_DirectoryRow.seatsFree = directorySeatsFree;
				m_DirectoryRow.joinMode = NetIceRowJoinMode(m_IceEnabled, !m_DirectoryRow.listenAddrs.empty(), m_IceBoundSessionId, m_Directory.GetSessionId());
				advertised = m_DirectoryRow;
				// A register receives a new id; only the existing bound row can advertise ICE.
				advertised.joinMode = NetIceRowJoinMode(m_IceEnabled, !advertised.listenAddrs.empty(), m_IceBoundSessionId, std::string());
			}
			m_Directory.Advertise(advertised, directoryRunning, directoryListed);
		} else if (!directoryWanted) {
			m_Directory.Retract();
			if (s_PortMapRequested) {
				s_PortMap.Release();
			}
		}
		m_Directory.Update(nowMs);
		{
			// The worker cannot touch the directory client, so what it needs is published here.
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_DirectorySessionId = m_Directory.GetSessionId();
			m_DirectoryToken = m_Directory.GetToken();
			m_DirectoryRegistered = m_Directory.GetState() == NetDirectoryClient::State::Registered;
		}
		// The world's image follows the writer thread, never a file read on this one.
		if (m_IsHost) {
			PublishFinishedWorldJoinImage();
		}
		PersistWorldDirectoryToken();
		if (m_WorldCatchUp.active) {
			PumpSessionEvents();
		}
		DriveReconnectUx(nowMs);
		// Last: the expiry destroys the service, so nothing in this pump may run after it.
		UpdateCompletedLobbyExpiry(nowMs);
	}

	bool NetMatchService::RematchLobbySeatedLocked() const {
		if (m_LobbySnapshot.members.empty()) {
			return false;
		}
		for (const NetLobbyMember& member: m_LobbySnapshot.members) {
			if (!member.cpu && !member.connected) {
				return false;
			}
		}
		return true;
	}

	// A rematch lobby lives only while its peers are coming back. One still a seat short after the
	// wait is destroyed here, which is what releases the session, the seats and the directory lease.
	void NetMatchService::UpdateCompletedLobbyExpiry(uint64_t nowMs) {
		bool expired = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_CompletedLobbySinceMs == 0) {
				return;
			}
			const bool settled = m_LeftMatch || m_State == NetMatchServiceState::Idle || m_State == NetMatchServiceState::Failed ||
			                     m_State == NetMatchServiceState::ReadyToLaunch || m_State == NetMatchServiceState::Running ||
			                     RematchLobbySeatedLocked();
			if (settled) {
				m_CompletedLobbySinceMs = 0;
				return;
			}
			expired = nowMs >= m_CompletedLobbySinceMs + c_CompletedLobbyExpiryMs;
			if (expired) {
				m_CompletedLobbySinceMs = 0;
			}
		}
		if (!expired) {
			return;
		}
		std::cout << "[net-match] rematch lobby expired after " << c_CompletedLobbyExpiryMs / 1000 << "s waiting for the other player" << std::endl;
		Destroy();
		SetState(NetMatchServiceState::Idle, "Idle", "The rematch lobby timed out.");
	}

	uint64_t NetMatchService::AdmissionNowMs() const {
		return m_AdmissionClock.NowMs(SteadyNowMs());
	}

	bool NetMatchService::WasJoinRefusedByALiveMatch() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_JoinRefusedByLiveMatch && m_State == NetMatchServiceState::Failed;
	}

	bool NetMatchService::BeginSubstituteApplication(const NetMatchServiceRequest& request, std::string* error) {
		s_ApplyOnce = true;
		if (Start(request, error)) {
			return true;
		}
		s_ApplyOnce = false;
		return false;
	}

	bool NetMatchService::NeedsCompletedLobbyPump() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		// The lobby a match end opens outlives the Completed state: until every peer is back it still
		// owes its lease a heartbeat and the peer that never returns its expiry.
		return !m_LeftMatch && (m_State == NetMatchServiceState::Completed || m_CompletedLobbySinceMs != 0);
	}

	bool NetMatchService::NeedsRecoveryPump() const {
		if (!s_AdmissionEnabled) {
			return false;
		}
		const NetReconnectUxState uxState = m_ReconnectUx.GetState();
		if (uxState == NetReconnectUxState::Waiting || uxState == NetReconnectUxState::Retrying) {
			return true;
		}
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_State != NetMatchServiceState::Failed || m_IsHost || !m_MatchWasRunning) {
			return false;
		}
		return m_TicketStore.HasRecord();
	}

	void NetMatchService::DriveReconnectUx(uint64_t nowMs) {
		NetMatchServiceState state = NetMatchServiceState::Idle;
		bool isHost = false;
		bool hasRecord = false;
		bool matchWasRunning = false;
		std::string reason;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			state = m_State;
			isHost = m_IsHost;
			hasRecord = m_TicketStore.HasRecord();
			matchWasRunning = m_MatchWasRunning;
			reason = m_ErrorText;
		}
		if (state == NetMatchServiceState::Running || state == NetMatchServiceState::ReadyToLaunch) {
			if (m_ReconnectUx.IsActive()) {
				m_ReconnectUx.NoteReconnected(nowMs);
			} else if (m_ReconnectUx.GetState() == NetReconnectUxState::Idle) {
				m_ReconnectUx.NoteConnected(nowMs);
			}
			return;
		}
		// §11's same-process loss: the link died mid-MATCH and we still hold the record that proves the
		// seat. A lobby that never started is not a match to reclaim; losing one goes back to the menu.
		if (!s_AdmissionEnabled || state == NetMatchServiceState::Starting) {
			return;
		}
		if (!NetReconnectUx::RecoveryApplies(state == NetMatchServiceState::Failed, isHost, hasRecord, matchWasRunning)) {
			return;
		}
		if (m_ReconnectUx.GetState() == NetReconnectUxState::Retrying) {
			m_ReconnectUx.NoteAttemptFailed(nowMs, reason);
		} else {
			m_ReconnectUx.NoteDropped(nowMs, reason);
		}
		if (!m_ReconnectUx.Tick(nowMs)) {
			return;
		}
		m_ReconnectUx.NoteAttemptStarted(nowMs);
		std::string attemptError;
		if (!BeginTicketRejoin(&attemptError)) {
			m_ReconnectUx.NoteAttemptFailed(nowMs, attemptError);
		}
	}

	void NetMatchService::SetReady() {
		m_ReadyRequested.store(true);
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_State == NetMatchServiceState::Starting) {
			m_StatusText = "Ready; waiting for host start";
			m_ErrorText.clear();
		}
	}

	void NetMatchService::RequestStart() {
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_State == NetMatchServiceState::Idle) {
				m_StatusText = "Host a match before starting";
				m_ErrorText.clear();
				return;
			}
			if (!m_IsHost) {
				m_StatusText = "Only the host can start the match";
				m_ErrorText.clear();
				return;
			}
			if (m_State == NetMatchServiceState::Starting) {
				m_StatusText = "Start requested; waiting for peer";
				m_ErrorText.clear();
			}
		}
		m_StartRequested.store(true);
	}

	bool NetMatchService::ConsumeReadyToLaunch(std::string& outActivityPreset) {
		Update();
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_State != NetMatchServiceState::ReadyToLaunch || !m_Coordinator) {
			return false;
		}
		ScenarioRunner::SetLockstepCoordinator(m_Coordinator.get(), m_PendingResyncState.has_value());
		m_Coordinator->DeferStopsToTickBoundary();
		m_Coordinator->SetSeatStateSource(&NetMatchService::QuerySeatState, this);
		// The lockstep wait parks the sim thread; without this the plane could not answer a leave or a
		// reclaim while the round waits on the very peer that sent it.
		ScenarioRunner::SetSessionPump([this] { PumpSessionEvents(); });
		ScenarioRunner::SetLockstepSeatPresence(&m_SeatPresence);
		// The coordinator owns the transport queue during the match; reconnect handshakes hand over
		// here and drain through PumpSessionEvents on the same (game) thread.
		DiscardUndeliveredSessionEventsLocked();
		AttachCoordinatorSessionSink();
		if (m_Runner) {
			std::string recordError;
			(void)ScenarioRunner::BeginLockstepReplayRecord(m_Runner->GetMatchConfig(), &recordError);
		}
		// The launch names the adopted activity: a joining peer's own request carries only its local default.
		const std::string& adopted = m_Runner ? m_Runner->GetMatchConfig().activityPreset : m_ActivityPreset;
		outActivityPreset = adopted.empty() ? m_ActivityPreset : adopted;
		m_MatchWasRunning = true;
		if (!m_PendingResyncState.has_value()) {
			ResetRosterTransitionHistory();
			if (m_Runner && m_Runner->GetMatchConfig().persistentWorld && !m_Runner->GetMatchConfig().worldId.empty()) {
				m_AutosaveMatchId = m_Runner->GetMatchConfig().worldId;
			} else {
				m_AutosaveMatchId = m_Runner ? std::format("{:x}-{:x}-{:x}", m_Runner->GetMatchConfig().sessionId, System::GetProcessID(),
				                                        std::chrono::system_clock::now().time_since_epoch().count()) : "";
			}
			m_MatchAutosaveSeconds = m_Runner ? MatchAutosaveSeconds(m_Runner->GetMatchConfig()) : 0;
			m_NextAutosaveSimTime = -1;
			m_LastAutosaveSimTime = -1;
		}
		if (!m_PendingResyncState.has_value() || m_CurrentMatchSummary.peers.empty()) {
			m_LastMatchSummary.reset();
			m_CurrentMatchSummary = {};
			m_SummarySeats = m_SeatPresence.GetSeats();
			const auto& config = m_Coordinator->GetConfig().matchConfig;
			for (const auto& slot : config.players) {
				if (slot.cpu) continue;
				std::string name = slot.displayName;
				for (const auto& member : m_LobbySnapshot.members) {
					if (member.peerId == slot.peerId) name = member.displayName;
				}
				m_CurrentMatchSummary.peers.push_back({slot.peerId, name, slot.team, static_cast<uint16_t>(slot.peerId - 1), NetMatchConfigUtil::PeerInputDelay(config, slot.peerId)});
			}
			m_CurrentMatchSummary.identityLine = "Exe: " + std::filesystem::path(System::GetThisExePathAndName()).filename().string() + " v" + c_GameVersion.str();
		} else {
			++m_CurrentMatchSummary.resyncs;
		}
		m_State = NetMatchServiceState::Running;
		m_StatusText = "Match running";
		const NetLockstepConfig& config = m_Coordinator->GetConfig();
		std::cout << std::format("[net-lockstep] start round={} frame={} local_peer={} peers={} input_delay={}\n",
		                         m_Coordinator->GetRoundId(), config.startFrame, config.localPeerId, config.peerCount, config.inputDelayFrames) << std::flush;
		CaptureA7SeatView();
		return true;
	}

	void NetMatchService::AutosaveAtTickBoundary(uint64_t tick) {
		if (m_WorldJoin.IsConfigured() && m_Coordinator) {
			NetLockstepReadyFrame ready;
			if (m_Coordinator->PeekReadyFrame(tick, ready)) {
				(void)m_WorldJoin.Tail().Append(PackWorldJoinReadyFrame(ready), nullptr);
			}
		}
		// Every peer keeps the schedule the host announced in the agreed config, not its own setting.
		const uint32_t seconds = m_MatchAutosaveSeconds;
		if (seconds == 0 || !ScenarioRunner::IsLockstepControllerSyncActive() ||
		    !g_ActivityMan.ActivityRunning() || m_AutosaveMatchId.empty()) return;
		const int64_t now = g_TimerMan.GetSimTimeTicks();
		const int64_t interval = static_cast<int64_t>(seconds) * g_TimerMan.GetTicksPerSecond();
		if (m_NextAutosaveSimTime < 0 || now < m_LastAutosaveSimTime) {
			m_NextAutosaveSimTime = now - g_TimerMan.GetDeltaTimeTicks() + interval;
		}
		m_LastAutosaveSimTime = now;
		if (now < m_NextAutosaveSimTime) return;
		m_NextAutosaveSimTime += ((now - m_NextAutosaveSimTime) / interval + 1) * interval;
		if (!g_ActivityMan.SaveAutosaveSnapshot(m_AutosaveMatchId, tick)) {
			return;
		}
		if (m_WorldJoin.IsConfigured()) {
			// The image is published when the writer thread has finished this archive, from the pump.
			std::cout << "[net-world] metrics " << m_WorldJoin.Metrics().BuildReportJson() << std::endl;
		}
	}

	NetWorldCheckpointImage NetMatchService::WorldImageFromAutosave(const ActivityMan::CompletedAutosave& entry, const NetWorldIdentity& identity,
	                                                             const NetMatchConfig& matchConfig, uint64_t membershipRevision, double captureMs) {
		NetWorldCheckpointImage image;
		// Everything here comes from what the writer thread finished: no file is read to build it.
		if (entry.archive == nullptr || entry.archive->empty() || entry.bytes != entry.archive->size() || entry.digest.empty()) {
			return image;
		}
		image.worldId = identity.worldId;
		image.boot = identity.boot;
		image.round = identity.round;
		image.tick = entry.tick;
		image.configRevision = matchConfig.configRevision;
		image.membershipRevision = membershipRevision;
		image.matchConfigHash = NetIdentity::HashHex(NetMatchConfigUtil::HashConfig(matchConfig));
		image.path = entry.path;
		image.bytes = entry.bytes;
		image.captureMs = captureMs;
		image.digest = entry.digest;
		return image;
	}

	void NetMatchService::PublishFinishedWorldJoinImage() {
		if (!m_WorldJoin.IsConfigured()) {
			return;
		}
		std::ifstream in(g_ActivityMan.LastAutosavePath(), std::ios::binary);
		std::vector<uint8_t> archive;
		if (in) {
			archive.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
		const NetWorldCheckpointImage image = WorldImageFromAutosave(*entry, m_WorldIdentity, m_MatchConfig,
		                                                            m_WorldJoin.Membership().Revision(), g_ActivityMan.LastAutosaveCaptureMs());
		if (!image.IsValid()) {
			return;
		}
		NetWorldCheckpointImage image;
		image.worldId = m_WorldIdentity.worldId;
		image.boot = m_WorldIdentity.boot;
		image.round = m_WorldIdentity.round;
		image.tick = tick;
		image.configRevision = m_MatchConfig.configRevision;
		image.membershipRevision = m_WorldJoin.Membership().Revision();
		image.matchConfigHash = NetIdentity::HashHex(NetMatchConfigUtil::HashConfig(m_MatchConfig));
		image.path = g_ActivityMan.LastAutosavePath();
		image.bytes = archive.size();
		image.captureMs = g_ActivityMan.LastAutosaveCaptureMs();
		image.digest = DigestWorldJoinBytes(archive);
		// The bytes every bootstrap ships are kept here: the file is read and hashed once per image,
		// not once per bootstrap per sim tick.
		m_WorldJoinImageDigest = image.digest;
		m_WorldJoinImageArchive = entry->archive;
		m_WorldJoin.PublishImage(image);
		std::cout << "[net-world] offer " << EncodeWorldJoinOffer(image) << std::endl;
	}

	NetPeerId NetMatchService::ResolveWorldReportConnection(const NetWorldJoinHost& host, const std::vector<NetSessionPeerInfo>& readyPeers, uint8_t fromPeer) {
		for (const NetWorldJoinSession& session: host.Sessions()) {
			if (session.assignedPeerId == fromPeer || WorldJoinLobbyPeer(session) == fromPeer) {
				return session.connection;
			}
		}
		for (const NetSessionPeerInfo& peer: readyPeers) {
			if (peer.transportPeerId == static_cast<NetPeerId>(fromPeer)) {
				return peer.transportPeerId;
			}
		}
		return c_InvalidNetPeerId;
	}

	void NetMatchService::ApplyWorldJoinReport(NetLobbySession& lobby, NetWorldJoinHost& host, const NetLobbySession::WorldJoinReport& report, NetPeerId connection, uint64_t nowFrame, uint64_t nowMs) {
		if (connection == c_InvalidNetPeerId) {
			return;
		}
		if (report.kind == c_NetWorldReportProgress) {
			const uint16_t received = static_cast<uint16_t>(report.value & 0xFFFFU);
			const uint16_t total = static_cast<uint16_t>(report.value >> 32);
			(void)host.NoteTransferProgress(connection, received, total);
			if (total != 0 && received >= total && host.Image().IsValid()) {
				(void)host.NoteTransferComplete(connection, host.Image().bytes, nullptr);
			}
		} else if (report.kind == c_NetWorldReportCatchUp) {
			uint64_t activation = 0;
			const NetWorldJoinSession* prior = host.FindSession(connection);
			const uint64_t previous = prior ? prior->acknowledgedThrough : 0;
			const uint64_t lastMs = prior ? prior->lastCatchUpReportMs : 0;
			const uint64_t ticks = report.value > previous ? report.value - previous : 0;
			const uint64_t elapsed = (lastMs != 0 && nowMs > lastMs) ? nowMs - lastMs : 1;
			(void)host.NoteCatchUpProgress(connection, report.value, ticks, elapsed, nowFrame, &activation, nullptr);
			host.NoteCatchUpClock(connection, nowMs);
			if (activation != 0) {
				if (const NetWorldJoinSession* session = host.FindSession(connection); session) {
					(void)lobby.SendPayloadTo(WorldJoinLobbyPeer(*session), MakeWorldJoinReport(c_NetWorldReportActivate, activation), nullptr);
				}
			}
		}
	}

	void NetMatchService::PumpWorldJoinLobby(uint64_t nowMs) {
		if (!m_Runner || !m_Session || !m_Coordinator) {
			return;
		}
		NetLobbySession& lobby = m_Runner->GetLobbySession();
		std::vector<NetTransportEvent> events;
		events.swap(m_PendingLobbyEvents);
		m_PendingLobbyBytes = 0;
		m_PendingLobbyOverflow = false;
		for (const NetTransportEvent& event: events) {
			lobby.HandleTransportEvent(event, nowMs);
		}
		lobby.PumpOutgoingChunks();
		const std::vector<NetSessionPeerInfo> readyPeers = m_Session->GetReadyPeers();
		const uint64_t nowFrame = m_Coordinator->GetStats().nextFrame;
		while (true) {
			const NetLobbySession::WorldJoinReport report = lobby.TakeWorldJoinReport();
			if (!report.pending) {
				break;
			}
			ApplyWorldJoinReport(lobby, m_WorldJoin, report, ResolveWorldReportConnection(m_WorldJoin, readyPeers, report.fromPeer), nowFrame, nowMs);
		}
	}

	bool NetMatchService::StartJoinerImageTransfer(const NetWorldJoinSession& session, std::string* error, bool* outUnstartable) {
		if (outUnstartable) *outUnstartable = false;
		if (!m_Runner || !m_WorldJoin.Image().IsValid()) {
			if (error) *error = "the joiner has no image yet";
			return false;
		}
		// Every cheap refusal is answered before the archive is touched: this runs on the sim thread
		// inside the tick, once per bootstrap per pump.
		if (std::string reason; !WorldBootstrapCanStart(session, &reason)) {
			if (error) *error = reason;
			// Nothing about this bootstrap can change: it holds no seat and the pool is full.
			if (outUnstartable) *outUnstartable = true;
			m_WorldJoin.Metrics().NoteBootstrapStall();
			return false;
		}
		const uint8_t lobbyPeer = WorldJoinLobbyPeer(session);
		NetLobbySession& lobby = m_Runner->GetLobbySession();
		std::string bindError;
		// The bind is idempotent: a known remote only has its transport re-pointed, so a retried
		// transfer costs nothing here.
		if (!lobby.BindWorldTransferRemote(lobbyPeer, session.connection, &bindError)) {
			if (error) *error = bindError;
			return false;
		}
		// Once per bootstrap, not once per retry: the seat's config does not change while it waits.
		if (session.assignedPeerId != 0 && m_WorldJoin.NoteMatchConfigSent(session.connection)) {
			lobby.SendMatchConfigTo(lobbyPeer);
		}
		// The bytes and their digest were read once, when the image was published; re-reading the
		// archive per pump is a sim-thread stall the world pays for every waiting bootstrap.
		if (m_WorldJoinImageArchive == nullptr || m_WorldJoinImageArchive->empty() || m_WorldJoinImageDigest != m_WorldJoin.Image().digest) {
			if (error) *error = "the published archive is missing or its digest does not match";
			m_WorldJoin.Metrics().NoteBootstrapStall();
			return false;
		}
		std::vector<std::vector<uint8_t>> tail;
		uint64_t lastCopied = 0;
		(void)m_WorldJoin.Tail().CopyFrom(m_WorldJoin.Image().tick + 1, 512, 1024ULL * 1024ULL, tail, &lastCopied);
		std::vector<uint8_t> blob;
		if (!EncodeWorldJoinImageBlob(m_WorldJoin.Image(), *m_WorldJoinImageArchive, tail, blob, error)) {
			return false;
		}
		// A queued image is the lobby's now: the bootstrap must not build the blob again next pump.
		// A refusal is not a start, so the next pump retries instead of waiting out the deadline.
		const NetLobbyStateTransfer outcome = lobby.BeginStateTransferToPeer(lobbyPeer, std::move(blob));
		if (outcome == NetLobbyStateTransfer::Refused && error) {
			*error = "the lobby refused the joiner image";
		}
		return NoteImageTransferOutcome(outcome, lobby, m_WorldJoin, session.connection, lastCopied);
	}

	bool NetMatchService::WorldBootstrapCanStart(const NetWorldJoinSession& session, std::string* reason) {
		if (WorldJoinLobbyPeer(session) == 0) {
			if (reason) *reason = "no world lobby id remains for this bootstrap";
			return false;
		}
		return true;
	}

	bool NetMatchService::NoteImageTransferOutcome(NetLobbyStateTransfer outcome, NetLobbySession& lobby, NetWorldJoinHost& host, NetPeerId connection, uint64_t deliveredThrough) {
		if (outcome == NetLobbyStateTransfer::Refused) {
			// Never taken and never kept, so the bootstrap stays unstarted and the next pump retries.
			return false;
		}
		const bool onThePump = outcome == NetLobbyStateTransfer::Started;
		const uint64_t transferId = onThePump ? lobby.GetOutgoingStateId() : 0;
		const uint16_t chunkCount = onThePump ? lobby.GetOutgoingChunkCount() : 0;
		return host.NoteTransferStarted(connection, transferId, chunkCount, deliveredThrough);
	}

	void NetMatchService::SendWorldJoinTailTo(NetLobbySession& lobby, NetWorldJoinHost& host, const NetWorldJoinSession& session) {
		if (session.phase != NetWorldJoinPhase::CatchingUp) {
			return;
		}
		std::vector<std::vector<uint8_t>> tail;
		uint64_t lastCopied = 0;
		if (host.Tail().CopyFrom(session.deliveredThrough + 1, 32, 40ULL * 1024ULL, tail, &lastCopied) == 0) {
			return;
		}
		std::vector<uint8_t> packed;
		for (const std::vector<uint8_t>& frame: tail) {
			const uint32_t size = static_cast<uint32_t>(frame.size());
			packed.push_back(static_cast<uint8_t>(size));
			packed.push_back(static_cast<uint8_t>(size >> 8));
			packed.push_back(static_cast<uint8_t>(size >> 16));
			packed.push_back(static_cast<uint8_t>(size >> 24));
			packed.insert(packed.end(), frame.begin(), frame.end());
		}
		if (packed.empty() || packed.size() > NetLobbyProtocol::c_MaxStateChunkBytes) {
			return;
		}
		NetLobbyStateChunk chunk;
		chunk.transferId = c_NetWorldTailTransferId;
		chunk.totalBytes = static_cast<uint32_t>(packed.size());
		chunk.chunkIndex = 0;
		chunk.chunkCount = 1;
		chunk.bytes = std::move(packed);
		const uint8_t lobbyPeer = WorldJoinLobbyPeer(session);
		if (lobbyPeer == 0) {
			return;
		}
		if (lobby.SendPayloadTo(lobbyPeer, chunk, nullptr)) {
			(void)host.NoteDeliveredThrough(session.connection, lastCopied);
		}
	}

	void NetMatchService::DriveWorldJoins(uint64_t nowMs) {
		if (!m_WorldJoin.IsConfigured() || !m_Coordinator || !m_Session) {
			return;
		}
		m_WorldJoin.ExpireStaleJoins(nowMs);
		// Every activation this pump announces is chosen ahead of what the round has already sent.
		m_WorldJoin.NoteSentInputThrough(m_Coordinator->SentInputThrough());
		PumpWorldJoinLobby(nowMs);
		const std::vector<NetSessionPeerInfo> readyPeers = m_Session->GetReadyPeers();
		std::vector<NetPeerId> liveConnections;
		liveConnections.reserve(readyPeers.size());
		for (const NetSessionPeerInfo& peer: readyPeers) {
			liveConnections.push_back(peer.transportPeerId);
		}
		m_WorldJoin.ReleaseLostConnections(liveConnections);
		for (const NetSessionPeerInfo& peer: readyPeers) {
			if (m_Coordinator->UsesTransportPeer(peer.transportPeerId)) {
				continue;
			}
			if (m_WorldJoin.FindSession(peer.transportPeerId) != nullptr) {
				continue;
			}
			const uint16_t stableSeat = m_ReconnectHost.StableSeatOfConnection(peer.transportPeerId);
				// The capture is taken here; the image is published once the writer has the archive, so the
				// bootstrap waits in SnapshotTransfer for a pump or two instead of reading a half-written file.
			if (stableSeat == 0) {
				continue;
			}
			if (!m_WorldJoin.BeginJoin(peer.transportPeerId, stableSeat, peer.displayName, nowMs, nullptr)) {
				continue;
			}
			const uint64_t tick = m_Coordinator->GetStats().nextFrame > 0 ? m_Coordinator->GetStats().nextFrame - 1 : 0;
			if (tick != 0 && m_WorldJoin.Image().tick != tick) {
				g_ActivityMan.SaveAutosaveSnapshot(m_AutosaveMatchId, tick);
				PublishWorldJoinImage(tick);
			}
			if (const NetWorldJoinSession* session = m_WorldJoin.FindSession(peer.transportPeerId)) {
				if (m_Runner) {
					const uint8_t lobbyPeer = WorldJoinLobbyPeer(*session);
					if (lobbyPeer != 0) {
						(void)m_Runner->GetLobbySession().BindLateRemote(lobbyPeer, peer.transportPeerId, nullptr);
						if (session->assignedPeerId != 0) {
							m_Runner->GetLobbySession().SendMatchConfigTo(lobbyPeer);
						}
					}
				}
			}
			std::cout << "[net-world] join connection=" << peer.transportPeerId << " name=" << peer.displayName << std::endl;
		}
		std::vector<std::pair<NetPeerId, std::string>> unstartable;
		for (const NetWorldJoinSession& session: m_WorldJoin.Sessions()) {
			if (session.phase == NetWorldJoinPhase::SnapshotTransfer && !session.transferStarted && m_WorldJoin.Image().IsValid()) {
				std::string transferError;
				bool cannotStart = false;
				if (!StartJoinerImageTransfer(session, &transferError, &cannotStart)) {
					std::cout << "[net-world] transfer wait connection=" << session.connection << " " << transferError << std::endl;
					if (cannotStart) {
						unstartable.emplace_back(session.connection, transferError);
					}
				}
			}
			if (session.phase == NetWorldJoinPhase::CatchingUp && m_Runner) {
				SendWorldJoinTailTo(m_Runner->GetLobbySession(), m_WorldJoin, session);
			}
		}
		// After the walk: CancelJoin erases from the vector the loop above is iterating.
		for (const auto& [connection, reason]: unstartable) {
			m_WorldJoin.CancelJoin(connection, reason);
			std::cout << "[net-world] cancel bootstrap connection=" << connection << " " << reason << std::endl;
		}
		if (m_Runner) {
			m_Runner->GetLobbySession().PumpOutgoingChunks();
		}
		const uint64_t nowFrame = m_Coordinator->GetStats().nextFrame;
		for (const NetWorldJoinSession& session: m_WorldJoin.Sessions()) {
			if (session.spectator && session.activationTick == 0) {
				(void)m_WorldJoin.ScheduleSpectatorActivation(session.connection, nowFrame, nullptr, nullptr);
			}
		}
		while (const NetWorldJoinSession* slow = m_WorldJoin.SlowActivation(nowFrame)) {
			uint64_t later = 0;
			if (slow->activationReannounces < c_NetWorldActivationReannounceLimit &&
			    m_WorldJoin.ReannounceActivation(slow->connection, nowFrame, &later, nullptr)) {
				if (m_Runner) {
					(void)m_Runner->GetLobbySession().SendPayloadTo(WorldJoinLobbyPeer(*slow), MakeWorldJoinReport(c_NetWorldReportActivate, later), nullptr);
				}
				std::cout << "[net-world] reannounce peer=" << static_cast<int>(slow->assignedPeerId) << " e=" << later << std::endl;
				continue;
			}
			// CancelJoin erases the session this pointer names, so the id is read before the call.
			const NetPeerId cancelled = slow->connection;
			m_WorldJoin.CancelJoin(cancelled, "the joiner missed the announced activation");
			std::cout << "[net-world] cancel slow join connection=" << cancelled << std::endl;
		}
		for (const NetH4SeatStatus& status: m_ReconnectHost.GetSeatStatuses()) {
			if (status.lockstepPeerId == 0 || status.committed || status.dropped || status.reclaiming) {
				continue;
			}
			const NetWorldSlot* slot = nullptr;
			for (const NetWorldSlot& candidate: m_WorldJoin.Membership().Slots()) {
				if (candidate.peerId == status.lockstepPeerId && candidate.held) {
					slot = &candidate;
					break;
				}
			}
			if (slot == nullptr) {
				continue;
			}
			bool active = false;
			NetPeerId connection = c_InvalidNetPeerId;
			for (const NetWorldJoinSession& session: m_WorldJoin.Sessions()) {
				if (session.assignedPeerId == status.lockstepPeerId && session.phase == NetWorldJoinPhase::Active) {
					active = true;
					connection = session.connection;
					break;
				}
			}
			if (!active) {
				continue;
			}
			NetGameWorldTransition release;
			release.kind = NetGameWorldTransition::Release;
			release.peerId = status.lockstepPeerId;
			release.holderGeneration = slot->generation;
			release.team = slot->team;
			(void)m_WorldJoin.Membership().Release(status.lockstepPeerId, nullptr);
			(void)ScenarioRunner::SubmitWorldTransition(release);
			m_WorldJoin.CancelJoin(connection, "clean leave");
			std::cout << "[net-world] release peer=" << static_cast<int>(status.lockstepPeerId) << std::endl;
		}
		const uint64_t nextFrame = m_Coordinator->GetStats().nextFrame;
		const NetWorldJoinSession* due = m_WorldJoin.DueActivation(nextFrame);
		bool late = false;
		if (due == nullptr) {
			due = m_WorldJoin.LateActivation(nextFrame);
			late = due != nullptr;
		}
		if (due != nullptr) {
			const NetWorldActivationPlan plan = PlanWorldActivation(*due, nextFrame, late);
			if (!plan.admit) {
				(void)m_WorldJoin.CompleteActivation(due->connection, plan.firstRequired, nullptr);
				std::cout << "[net-world] spectator stream at=" << plan.firstRequired << std::endl;
				return;
			}
			std::string admitError;
			if (!m_Coordinator->AdmitWorldMember(due->assignedPeerId, due->connection, plan.firstRequired, &admitError)) {
				m_WorldJoin.CancelJoin(due->connection, admitError);
				return;
			}
			if (plan.submitTransition) {
				NetGameWorldTransition transition = BuildWorldActivateTransition(*due, m_Runner ? m_Runner->GetMatchConfig() : m_MatchConfig, m_WorldJoin.Membership().Revision());
				transition.activationFrame = plan.firstRequired;
				(void)ScenarioRunner::SubmitWorldTransition(transition);
			}
			(void)m_WorldJoin.CompleteActivation(due->connection, plan.firstRequired, nullptr);
			std::cout << "[net-world] activate peer=" << static_cast<int>(due->assignedPeerId) << " at=" << plan.firstRequired << std::endl;
		}
	}

	bool NetMatchService::PrepareReceivedWorldJoin(const std::vector<uint8_t>& bytes, std::string& pendingLoad, std::string* error) {
		NetWorldCheckpointImage image;
		std::vector<uint8_t> archive;
		std::vector<std::vector<uint8_t>> tailBytes;
		if (!DecodeWorldJoinImageBlob(bytes, image, archive, tailBytes, error)) {
			return false;
		}
		const auto name = std::string("world_join_recv");
		const auto path = g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/" + name + ".ccsave";
		std::error_code directoryCode;
		std::filesystem::create_directories(std::filesystem::path(path).parent_path(), directoryCode);
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size()));
		out.close();
		if (!out.good()) {
			if (error) *error = "could not write the received world join archive";
			return false;
		}
		m_WorldCatchUp = {};
		m_WorldCatchUp.active = true;
		m_WorldCatchUp.snapshotTick = image.tick;
		m_WorldCatchUp.appliedThrough = image.tick;
		m_WorldCatchUp.digest = image.digest;
		for (const std::vector<uint8_t>& encoded: tailBytes) {
			NetLockstepFrame frame;
			NetLockstepError decodeError;
			if (!NetLockstepCodec::DecodeRecoveryInput(encoded, frame, &decodeError)) {
				if (error) *error = "world join tail did not decode: " + decodeError.message;
				return false;
			}
			m_WorldCatchUp.tail.push_back(std::move(frame));
		}
		pendingLoad = name;
		return true;
	}

	void NetMatchService::StepWorldJoinCatchUpClient(NetLobbySession& lobby, NetWorldCatchUpClient& catchUp) {
		std::vector<uint8_t> packed = lobby.TakePendingTailBytes();
		std::vector<NetLockstepFrame> later;
		size_t offset = 0;
		while (offset + 4 <= packed.size()) {
			const uint32_t size = static_cast<uint32_t>(packed[offset]) | (static_cast<uint32_t>(packed[offset + 1]) << 8) |
			                      (static_cast<uint32_t>(packed[offset + 2]) << 16) | (static_cast<uint32_t>(packed[offset + 3]) << 24);
			offset += 4;
			if (offset + size > packed.size()) {
				break;
			}
			NetLockstepFrame frame;
			if (NetLockstepCodec::DecodeRecoveryInput(std::vector<uint8_t>(packed.begin() + static_cast<std::ptrdiff_t>(offset),
			                                                              packed.begin() + static_cast<std::ptrdiff_t>(offset + size)),
			                                          frame, nullptr)) {
				later.push_back(frame);
				catchUp.tail.push_back(std::move(frame));
			}
			offset += size;
		}
		if (!later.empty()) {
			ScenarioRunner::AppendWorldCatchUp(std::move(later));
		}
		const NetLobbySession::WorldJoinReport report = lobby.TakeWorldJoinReport();
		if (report.pending && report.kind == c_NetWorldReportActivate) {
			catchUp.activationTick = report.value;
			ScenarioRunner::SetWorldCatchUpActivation(report.value);
		}
		catchUp.appliedThrough = std::max(catchUp.appliedThrough, ScenarioRunner::WorldCatchUpAppliedThrough());
		if (catchUp.appliedThrough > catchUp.snapshotTick) {
			(void)lobby.SendPayload(MakeJoinerCatchUpReport(), nullptr);
		}
	}

	void NetMatchService::PersistWorldDirectoryToken() {
		if (!m_WorldIdentity.IsValid() || m_Directory.GetState() != NetDirectoryClient::State::Registered) {
			return;
		}
		const std::string& issued = m_Directory.GetToken();
		if (issued.empty() || issued == m_WorldIdentity.directoryToken || m_WorldJoin.IdentityPath().empty()) {
			return;
		}
		// The token outlives this process: the next boot proves the row is the world's own with it.
		NetWorldIdentity stored = m_WorldIdentity;
		stored.directoryToken = issued;
		{
			// The live token is this process's the moment the service issues it; the record is only how
			// the NEXT boot proves the row is the world's, so the write follows the in-memory value.
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_WorldIdentity.directoryToken = issued;
			m_DirectoryRow.resumeToken = issued;
		}
		// One temp+rename of a ~100 byte record, once per issued token, never per tick. The service
		// has no IO thread to hand it to: m_Worker (NetMatchService.h:664) is the one-shot setup
		// thread WorkerMain/WorkerRematchMain/WorkerResyncMain own, and Update() itself runs on the
		// game thread inside the sim tick (Main.cpp:4520).
		std::string writeError;
		if (!NetWorldIdentityFile::Write(m_WorldJoin.IdentityPath(), stored, &writeError)) {
			std::cout << "[net-world] directory token not persisted: " << writeError << std::endl;
		}
	}

	bool NetMatchService::ReleaseWorldCatchUpOnceRunning(bool coordinatorRunning, NetWorldCatchUpClient& catchUp) {
		if (!coordinatorRunning || !catchUp.active) {
			return false;
		}
		// The coordinator owns the wire and the pacing from the moment it runs. A catch-up left armed
		// past that drains the round's own packets into the session and keeps reporting a finished
		// bootstrap the host answers with "that bootstrap is not catching up".
		catchUp = {};
		ScenarioRunner::ReleaseWorldCatchUp();
		return true;
	}

	void NetMatchService::DriveWorldJoinClient(uint64_t nowMs) {
		if (m_IsHost || !m_WorldCatchUp.active || !m_Runner) {
			return;
		}
		if (ReleaseWorldCatchUpOnceRunning(m_Coordinator != nullptr && m_Coordinator->IsRunning(), m_WorldCatchUp)) {
			return;
		}
		NetLobbySession& lobby = m_Runner->GetLobbySession();
		INetTransport* wire = ActiveWireLocked();
		if (wire) {
			for (const NetTransportEvent& event: wire->PollEvents()) {
				if (event.type == NetTransportEventType::PacketReceived && NetLobbyProtocol::Decode(event.bytes).ok) {
					lobby.HandleTransportEvent(event, nowMs);
				} else if (m_Session) {
					m_Session->InjectEvent(event, nowMs);
				}
			}
		}
		StepWorldJoinCatchUpClient(lobby, m_WorldCatchUp);
		// The joiner's lockstep starts here, after the E-1 frame has been applied, never from inside Take.
		if (m_WorldCatchUp.activationTick != 0 && m_WorldCatchUp.appliedThrough + 1 >= m_WorldCatchUp.activationTick && m_Coordinator &&
		    !m_Coordinator->IsRunning() && m_Session && wire) {
			std::string startError;
			const bool running = m_Runner->IsWorldJoinLockstepStarting()
			                         ? m_Runner->PumpWorldJoinLockstepStart(*m_Coordinator, &startError)
			                         : m_Runner->StartWorldJoinLockstep(*wire, *m_Session, *m_Coordinator, m_WorldCatchUp.activationTick, &startError);
			if (!running && !startError.empty()) {
				std::cout << "[net-world] joiner lockstep start: " << startError << std::endl;
			}
		}
	}

	void NetMatchService::PumpSeatPresence() {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_Coordinator || m_State != NetMatchServiceState::Running) {
			return;
		}
		if (const auto snapshot = m_Coordinator->TakeSeatSnapshot()) {
			if (m_SeatPresence.ApplySnapshot(*snapshot)) {
				RecordRosterTransitions(snapshot->observedAtMs);
			}
		}
		m_SeatPresence.NoteFrame(ScenarioRunner::GetLockstepAppliedFrame());
		CaptureA7SeatView();
	}

	void NetMatchService::PublishModerationView() {
		if (!m_Coordinator || !m_IsHost || !m_AdmissionAttached || m_State != NetMatchServiceState::Running) return;
		auto seats = m_ReconnectHost.GetModerationView();
		const uint64_t appliedFrame = ScenarioRunner::GetLockstepAppliedFrame();
		const auto& leaves = m_Coordinator->GetPeerLeaveFrames();
		std::vector<NetSeatPresenceEntry> presence;
		for (auto& seat: seats) {
			if (seat.cpu || seat.lockstepPeerId == 0) continue;
			for (const auto& member: m_LobbySnapshot.members) {
				if (member.peerId == seat.lockstepPeerId) { seat.displayName = member.displayName; break; }
			}
			if (!seat.substituteName.empty()) seat.displayName = seat.substituteName;
			if (seat.displayName.empty()) {
				for (const auto& previous: m_ModerationSeats) {
					if (previous.stableSeat == seat.stableSeat && previous.epoch == seat.epoch) { seat.displayName = previous.displayName; break; }
				}
			}
			NetSeatPresenceEntry entry;
			entry.stableSeat = seat.stableSeat;
			entry.peerId = seat.lockstepPeerId;
			entry.holderName = seat.displayName;
			entry.holderGeneration = seat.holderGeneration;
			entry.seatGeneration = seat.seatGeneration;
			entry.incarnation = seat.incarnation;
			if (seat.closed) {
				entry.state = NetSeatPresenceState::Left;
			} else if (seat.dropped) {
				entry.state = seat.reclaiming ? NetSeatPresenceState::Reconnecting : NetSeatPresenceState::Disconnected;
				entry.holdActive = seat.heldForReclaim;
				entry.holdUntilMs = seat.holdUntilMs;
			} else if (!seat.substituteName.empty()) {
				entry.state = NetSeatPresenceState::Substituted;
			}
			const auto left = leaves.find(seat.lockstepPeerId);
			if (seat.dropped && left != leaves.end()) {
				entry.holdUntilFrame = left->second + NetLockstepCoordinator::c_ReclaimHoldFrames;
				seat.holdFramesRemaining = appliedFrame < entry.holdUntilFrame ? entry.holdUntilFrame - appliedFrame : 0;
			}
			presence.push_back(std::move(entry));
		}
		std::sort(presence.begin(), presence.end(), [](const auto& a, const auto& b) { return a.stableSeat < b.stableSeat; });
		m_ModerationSeats = std::move(seats);
		(void)m_Coordinator->PublishSeatSnapshot(std::move(presence), AdmissionNowMs());
		if (const auto snapshot = m_Coordinator->TakeSeatSnapshot()) {
			if (m_SeatPresence.ApplySnapshot(*snapshot)) {
				RecordRosterTransitions(snapshot->observedAtMs);
			}
		}
	}

	void NetMatchService::AttachCoordinatorSessionSink() {
		if (!m_Coordinator) {
			return;
		}
		m_Coordinator->SetSessionEventSink([this](const NetTransportEvent& event) {
			if (event.type == NetTransportEventType::PacketReceived && NetLobbyProtocol::Decode(event.bytes).ok) {
				QueueLobbyEvent(event);
			} else {
				m_PendingSessionEvents.push_back(event);
			}
		});
	}

	void NetMatchService::DrainPendingSessionEventsLocked(bool atTickBoundary) {
		if (m_PendingSessionEvents.empty() || !m_Session) {
			return;
		}
		std::vector<NetTransportEvent> events;
		events.swap(m_PendingSessionEvents);
		const uint64_t nowMs = AdmissionNowMs();
		// A drop recorded here walks g_MovableMan, which only a resync leaves standing at a finished tick.
		std::optional<SimCensusScope> census;
		if (atTickBoundary) {
			census.emplace();
		}
		for (const NetTransportEvent& event: events) {
			m_Session->InjectEvent(event, nowMs);
			++m_SessionEventsDrained;
		}
	}

	void NetMatchService::DiscardUndeliveredSessionEventsLocked() {
		if (!m_PendingSessionEvents.empty()) {
			m_SessionEventsDiscarded += static_cast<uint32_t>(m_PendingSessionEvents.size());
			std::cout << "[net-match] discarded " << m_PendingSessionEvents.size() << " undelivered session events" << std::endl;
			m_PendingSessionEvents.clear();
		}
	}

	void NetMatchService::AccumulateLockstepTotalsLocked() {
		if (!m_Coordinator) {
			return;
		}
		const NetLockstepStats& stats = m_Coordinator->GetStats();
		m_LockstepTotals.peerFramesWaived += stats.peerFramesWaived;
		m_LockstepTotals.peersDroppedSilent += stats.peersDroppedSilent;
		m_LockstepTotals.connectionsClosedOnEviction += stats.connectionsClosedOnEviction;
	}

	void NetMatchService::QueueLobbyEvent(const NetTransportEvent& event) {
		if (m_PendingLobbyOverflow) return;
		if (m_PendingLobbyEvents.size() >= 1024 || event.bytes.size() > 1024 * 1024 - m_PendingLobbyBytes) {
			m_PendingLobbyOverflow = true;
			return;
		}
		m_PendingLobbyBytes += event.bytes.size();
		m_PendingLobbyEvents.push_back(event);
	}

	void NetMatchService::PumpCompletedSessionLocked() {
		if (m_State != NetMatchServiceState::Completed || m_LeftMatch || m_Worker.joinable() || !m_Session) return;
		INetTransport* wire = ActiveWireLocked();
		if (!wire) return;
		const uint64_t nowMs = AdmissionNowMs();
		for (const NetTransportEvent& event : wire->PollEvents()) {
			if (event.type == NetTransportEventType::PacketReceived && NetLobbyProtocol::Decode(event.bytes).ok) {
				m_Session->NotePeerTraffic(event.peerId, nowMs);
				QueueLobbyEvent(event);
			} else if (event.type == NetTransportEventType::PacketReceived && NetLockstepCodec::LooksLikePacket(event.bytes)) {
				m_Session->NotePeerTraffic(event.peerId, nowMs);
				++m_EndedLockstepPackets;
			} else {
				m_Session->InjectEvent(event, nowMs);
			}
		}
		m_Session->Tick(nowMs, false);
		RefuseEndedPeersLocked("match over");
	}

	void NetMatchService::PumpSessionEvents() {
		PushPendingToasts();
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_State == NetMatchServiceState::Completed) {
				PumpCompletedSessionLocked();
				return;
			}
		}
		PumpSeatPresence();
		{
			// The stamp must track the sim even on ticks that carry no session events, or a send
			// between heartbeats would date a chat line by the last heartbeat's frame.
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_Session && m_Coordinator && m_State == NetMatchServiceState::Running) {
				m_Session->SetLockstepFrame(m_Coordinator->GetStats().nextFrame);
				m_LastRoundId = static_cast<uint32_t>(m_Coordinator->GetRoundId());
				m_ReconnectClient.SetRound(m_LastRoundId);
				// The coordinator owns the transport queue mid-match, so this pump is the only
				// driver that ever drains the session's chat outbox here.
				m_Session->PumpChatOutbox();
			}
		}
		const bool hostAdmission = m_AdmissionAttached && m_IsHost;
		const bool holdPause = m_Coordinator && m_Coordinator->AnyDroppedSeatHeld();
		const bool worldJoin = (m_Coordinator && m_Coordinator->IsPersistentWorldRound()) || m_WorldCatchUp.active;
		if (m_PendingSessionEvents.empty() && !hostAdmission && !holdPause && !worldJoin) {
			return;
		}
		std::vector<NetTransportEvent> events;
		events.swap(m_PendingSessionEvents);
		bool answerMatchOver = false;
		std::string rejoinName;
		{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_Session || m_State != NetMatchServiceState::Running) {
			return;
		}
		// This is the sim thread inside the tick, so the drop-frame ownership census may walk the world
		// here and nowhere else. The scope is what makes that a checked property rather than a comment.
		const SimCensusScope censusScope;
		// Chat entries stamp the frame they arrived on, on every peer, not only the host's plane.
		m_Session->SetLockstepFrame(m_Coordinator ? m_Coordinator->GetStats().nextFrame : 0);
		if (m_Coordinator) {
			m_LastRoundId = static_cast<uint32_t>(m_Coordinator->GetRoundId());
		}
		m_ReconnectClient.SetRound(m_Coordinator ? m_LastRoundId : 0);
		if (hostAdmission) {
			// Phase A: a ticketless join into a running match is refused; a returning holder proves.
			m_ReconnectHost.SetLiveMatch(true);
		}
		const uint64_t nowMs = AdmissionNowMs();
		// F1.5: the two clocks must be one. Sampled here, at the pump, because the report is written
		// after the loop stops feeding the session and its difference reads the teardown by then.
		if (const uint64_t sessionMs = m_Session->GetClockMs(); sessionMs > nowMs) {
			const uint64_t divergenceMs = sessionMs - nowMs;
			uint64_t seen = m_MaxClockDivergenceMs.load();
			while (divergenceMs > seen && !m_MaxClockDivergenceMs.compare_exchange_weak(seen, divergenceMs)) {
			}
		}
		if (hostAdmission) {
			// The coordinator owns the transport queue mid-match, so the session's own Tick never runs;
			// without this the plane's clock stops and a delayed refusal, an offer retransmit or a
			// dropped seat's reclaim window would wait for the match to end.
			m_Session->TickAdmissionPlane(nowMs);
		}
		if (holdPause && m_Session) {
			m_Session->TickKeepalive(nowMs);
		}
		for (const NetTransportEvent& event: events) {
			m_Session->InjectEvent(event, nowMs);
		}
		if (hostAdmission) {
			// The reseat is a lockstep command, not a local mutation: every peer applies the identical
			// ownership at the identical tick. QueueLocalInput restamps the sender as this peer, which
			// on the host is exactly the id MovableMan's reseat gate requires.
			DriveAutoSubstitution(nowMs);
			for (const NetGameReseat& reseat: m_ReconnectHost.TakePendingReseats()) {
				std::cout << "[net-reconnect] reseating team " << reseat.team << " onto peer "
				          << static_cast<int>(reseat.newOwnerPeerId) << " (" << reseat.actorUIDs.size() << " actors)" << std::endl;
				ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{ScenarioRunner::GetLockstepHostPeerId(), reseat});
			}
			if (m_Coordinator) {
				for (const NetHoldResolutionNotice& notice: m_ReconnectHost.TakePendingHoldResolutions()) {
					NetLockstepHoldResolution resolution = NetLockstepHoldResolution::None;
					switch (notice.resolution) {
						case NetHoldResolution::Expired: resolution = NetLockstepHoldResolution::Expired; break;
						case NetHoldResolution::Reclaimed: resolution = NetLockstepHoldResolution::Reclaimed; break;
						case NetHoldResolution::Substituted: resolution = NetLockstepHoldResolution::Substituted; break;
					}
					m_Coordinator->ResolveHeldSeat(notice.lockstepPeerId, resolution, nowMs);
				}
			}
			m_SeatStatuses = m_ReconnectHost.GetSeatStatuses();
			PublishModerationView();
			CaptureA7SeatView();
		}
		if (m_IsHost && m_Coordinator && m_Coordinator->IsRunning() && m_Coordinator->IsPersistentWorldRound()) {
			DriveWorldJoins(nowMs);
		}
		DriveWorldJoinClient(nowMs);
		if (events.empty()) {
			return;
		}
		// A transport peer that reached session-Ready but carries no lockstep remote is a
		// reconnector: the host ends the round so everyone reconvenes around its snapshot.
		// A persistent world never takes that path: a fresh join is a bootstrap, not a ResyncMatch.
		if (m_IsHost && m_ResyncOnDesync && m_Coordinator && m_Coordinator->IsRunning() && !m_Coordinator->IsPersistentWorldRound()) {
			for (const NetSessionPeerInfo& peer: m_Session->GetReadyPeers()) {
				if (!m_Coordinator->UsesTransportPeer(peer.transportPeerId)) {
					rejoinName = peer.displayName.empty() ? "a player" : peer.displayName;
					if (ClassifyRejoin(g_ActivityMan.GetActivity()) == NetRejoinAnswer::MatchOver) {
						answerMatchOver = true;
					} else {
						std::cout << "[net-match] rejoin: " << rejoinName << " reconnected - resyncing the match" << std::endl;
						m_Coordinator->RequestResync("player rejoined");
					}
					break;
				}
			}
		}
		}
		if (answerMatchOver) {
			std::cout << "[net-match] rejoin: " << rejoinName << " reconnected - match is over" << std::endl;
			AnswerMatchOverRejoin("match over");
		}
	}

	NetMatchServiceState NetMatchService::GetState() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_State;
	}

	NetMatchService::PortMapStatus NetMatchService::GetPortMapStatus() const {
		PortMapStatus status;
		status.enabled = s_PortMapRequested;
		status.done = s_PortMapRequested && s_PortMap.Done();
		status.mapped = s_PortMapRequested && s_PortMap.Mapped();
		if (status.done || status.mapped) {
			const NetPortMap::Result& result = s_PortMap.GetResult();
			status.method = NetPortMap::MethodName(result.method);
			status.externalIp = result.externalIp;
			status.externalPort = result.externalPort;
			status.error = result.error;
		}
		return status;
	}

	NetLobbySnapshot NetMatchService::GetLobbySnapshot() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		NetLobbySnapshot snapshot = m_LobbySnapshot;
		snapshot.serviceState = StateName(m_State);
		snapshot.statusText = m_StatusText;
		if (!m_ErrorText.empty()) snapshot.errorText = m_ErrorText;
		snapshot.isHost = m_IsHost;
		snapshot.localPeerId = m_LocalPeerId;
		snapshot.localTeam = m_LocalTeam;
		snapshot.active = m_State != NetMatchServiceState::Idle;
		snapshot.inLobby = m_State == NetMatchServiceState::Starting;
		snapshot.running = m_State == NetMatchServiceState::Running || m_State == NetMatchServiceState::ReadyToLaunch;
		snapshot.failed = m_State == NetMatchServiceState::Failed;
		snapshot.playedAMatch = m_MatchWasRunning;
		snapshot.inputDelayText = m_InputDelayText;
		if (snapshot.isHost && snapshot.active) {
			const PortMapStatus portMap = GetPortMapStatus();
			if (portMap.enabled) {
				if (portMap.mapped) {
					snapshot.portMap = "Public endpoint: " + portMap.externalIp + ":" + std::to_string(portMap.externalPort) + " via " + portMap.method;
				} else if (portMap.done) {
					snapshot.portMap = "No router mapping (" + (portMap.error.empty() ? "failed" : portMap.error) + ")";
				} else {
					snapshot.portMap = "Mapping the port...";
				}
			}
		}
		if (snapshot.portMap != s_PortMapLine) {
			s_PortMapLine = snapshot.portMap;
			++s_PortMapSerial;
		}
		snapshot.portMapSerial = s_PortMapSerial;
		if (snapshot.activityPreset.empty()) {
			snapshot.activityPreset = m_ActivityPreset;
		}
		if (snapshot.activityModule.empty()) {
			snapshot.activityModule = m_ActivityModule;
		}
		if (snapshot.sceneName.empty()) {
			snapshot.sceneName = m_SceneName;
		}
		if (snapshot.sceneModule.empty()) {
			snapshot.sceneModule = m_SceneModule;
		}
		if (snapshot.members.empty() && snapshot.active) {
			NetLobbyMember local;
			local.peerId = m_LocalPeerId;
			local.displayName = m_LocalName;
			local.team = m_LocalTeam >= 0 ? static_cast<uint8_t>(m_LocalTeam) : 0;
			local.isLocal = true;
			local.connected = true;
			snapshot.members.push_back(local);
		}
		for (NetLobbyMember& member: snapshot.members) {
			const auto current = m_SeatPresence.GetSeats().find(member.peerId);
			if (current != m_SeatPresence.GetSeats().end() && !current->second.holderName.empty()) member.displayName = current->second.holderName;
			const NetSeatPresenceState state = m_SeatPresence.StateOf(member.peerId);
			member.dropped = state == NetSeatPresenceState::Disconnected || state == NetSeatPresenceState::Reconnecting;
			member.reclaiming = state == NetSeatPresenceState::Reconnecting;
			member.statusLine = m_SeatPresence.Line(member.peerId, member.displayName);
		}
		return snapshot;
	}

	bool NetMatchService::SendChat(uint8_t scope, const std::string& text) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		// Only a live lobby or match can carry a line to the wire: after LeaveWorkerMain the
		// session object (and m_ChatSession) is still owned but no pump will ever drain it, so
		// accepting would just let the UI drop text it should have kept.
		if (m_State != NetMatchServiceState::Starting &&
		    m_State != NetMatchServiceState::ReadyToLaunch &&
		    m_State != NetMatchServiceState::Running) {
			return false;
		}
		return m_ChatSession && m_ChatSession->SendChat(scope, text);
	}

	std::vector<NetChatEntry> NetMatchService::TakeChatEntries() {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_ChatSession ? m_ChatSession->TakeChatEntries() : std::vector<NetChatEntry>{};
	}

	std::vector<NetChatEntry> NetMatchService::ChatHistory() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_ChatSession ? m_ChatSession->ChatHistory() : std::vector<NetChatEntry>{};
	}

	std::string NetMatchService::GetInputDelayText() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_InputDelayText;
	}

	std::optional<uint32_t> NetMatchService::GetMatchPingMs() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		const INetTransport* wire = ActiveWireLocked();
		if (m_State != NetMatchServiceState::Running || !wire || !m_Session) {
			return std::nullopt;
		}
		std::optional<uint32_t> ping;
		for (const NetSessionPeerInfo& peer: m_Session->GetReadyPeers()) {
			if (m_Coordinator && m_Coordinator->UsesTransportPeer(peer.transportPeerId)) {
				ping = std::max(ping.value_or(0), wire->GetPeerPingMs(peer.transportPeerId));
			}
		}
		return ping;
	}

	std::string NetMatchService::GetPeerDisplayName(uint8_t peerId) const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		const auto seat = m_SeatPresence.GetSeats().find(peerId);
		if (seat != m_SeatPresence.GetSeats().end() && !seat->second.holderName.empty()) {
			return seat->second.holderName;
		}
		for (const NetLobbyMember& member: m_LobbySnapshot.members) {
			if (member.peerId == peerId && !member.displayName.empty()) {
				return member.displayName;
			}
		}
		return "Player " + std::to_string(peerId);
	}

	bool NetMatchService::IsMatchResyncing() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_ResyncHealOpen && (m_State == NetMatchServiceState::Running ||
		       m_State == NetMatchServiceState::Starting || m_State == NetMatchServiceState::ReadyToLaunch);
	}

	std::string NetMatchService::GetStatusText() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_StatusText;
	}

	std::string NetMatchService::GetErrorText() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_ErrorText.empty() ? m_LobbySnapshot.errorText : m_ErrorText;
	}

	void NetMatchService::CacheDiagnosticIdentity(const NetIdentityManifest& manifest) {
		const auto& config = manifest.deterministicConfig;
		const json fields{
			{"game_version", config.gameVersion}, {"network_protocol_version", config.networkProtocolVersion},
			{"controller_frame_version", config.controllerFrameVersion}, {"controller_frame_encoded_size", config.controllerFrameEncodedSize},
			{"delta_time_bits", config.deltaTimeBits}, {"ai_update_interval", config.aiUpdateInterval},
			{"pathfinder_grid_node_size", config.pathfinderGridNodeSize}, {"recommended_moid_count", config.recommendedMoidCount},
			{"particle_settling", config.particleSettling}, {"mo_subtraction", config.moSubtraction},
			{"num_lua_states", config.numLuaStates}, {"num_lua_states_override", config.numLuaStatesOverride},
			{"selected_module", config.selectedModule}, {"scenario_test_module_loaded", config.scenarioTestModuleLoaded},
			{"lockstep_codec_version", config.lockstepCodecVersion}, {"enabled_global_scripts", config.enabledGlobalScripts}};
		const json identity{{"schema", manifest.schema}, {"build_id", manifest.buildId}, {"game_version", manifest.gameVersion},
			{"platform", manifest.platform}, {"deterministic_config", fields},
			{"module_manifest_hash", NetIdentity::HashHex(manifest.moduleManifestHash)},
			{"deterministic_config_hash", NetIdentity::HashHex(manifest.deterministicConfigHash)},
			{"session_rules_hash", NetIdentity::HashHex(manifest.sessionRulesHash)},
			{"session_identity_hash", NetIdentity::HashHex(manifest.sessionIdentityHash)}};
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_DiagnosticIdentity = identity.dump(2, ' ', false, json::error_handler_t::replace);
		++m_DiagnosticIdentityGeneration;
	}

	namespace {
		NetIdentityBuildOptions DiagnosticIdentityOptions() {
			NetIdentityBuildOptions options;
			options.buildId = "stage2-p2d-local";
			options.sessionRulesTag = "stage2-p2-session-rules";
			return options;
		}
	} // namespace

	bool NetMatchService::RefreshDiagnosticIdentity(std::string* error, double* buildMs) {
		NetIdentityManifest manifest;
		NetIdentityBuildOptions options = DiagnosticIdentityOptions();
		NetIdentity::StampOptionsForTarget(options, m_MatchConfig.persistentWorld);
		const auto started = std::chrono::steady_clock::now();
		const bool built = NetIdentity::BuildCurrentManifest(manifest, error, options);
		if (buildMs) *buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		if (!built) return false;
		CacheDiagnosticIdentity(manifest);
		return true;
	}

	bool NetMatchService::CaptureDiagnosticIdentityInputs(std::string* error) {
		NetIdentityManifest inputs;
		if (!NetIdentity::CaptureManifestInputs(inputs, error, DiagnosticIdentityOptions())) return false;
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_DiagnosticIdentityInputs = std::move(inputs);
		m_DiagnosticIdentityInputsPending = true;
		m_DiagnosticIdentityInputsGeneration = m_DiagnosticIdentityGeneration;
		return true;
	}

	void NetMatchService::DropCapturedDiagnosticIdentityInputs() {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_DiagnosticIdentityInputsPending = false;
		m_DiagnosticIdentityInputs = NetIdentityManifest{};
	}

	bool NetMatchService::BuildCapturedDiagnosticIdentity(std::string* error, double* buildMs) {
		NetIdentityManifest manifest;
		uint64_t capturedGeneration = 0;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (!m_DiagnosticIdentityInputsPending) {
				if (error) *error = "no captured identity inputs";
				return false;
			}
			manifest = m_DiagnosticIdentityInputs;
			capturedGeneration = m_DiagnosticIdentityInputsGeneration;
			m_DiagnosticIdentityInputsPending = false;
			m_DiagnosticIdentityInputs = NetIdentityManifest{};
		}
		const NetIdentityManifest captured = manifest;
		const auto started = std::chrono::steady_clock::now();
		// The game thread can write a module tree while this walk reads it, so one failed walk is retried
		// from the captured inputs before it is reported.
		bool built = NetIdentity::CompleteManifestFromInputs(manifest, error, DiagnosticIdentityOptions());
		if (!built) {
			manifest = captured;
			built = NetIdentity::CompleteManifestFromInputs(manifest, error, DiagnosticIdentityOptions());
		}
		if (buildMs) *buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		if (!built) return false;
		{
			// A newer identity was cached while this one hashed: that one is the current build, not this.
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_DiagnosticIdentityGeneration != capturedGeneration) return true;
		}
		CacheDiagnosticIdentity(manifest);
		return true;
	}

	std::string NetMatchService::ExportDiagnosticIdentity() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_DiagnosticIdentity;
	}

	std::string NetMatchService::ExportDiagnosticDesyncHeal() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		json record{{"present", !m_DiagnosticRuntimeError.empty() || m_LastResync.happened || m_ResyncHealOpen},
		            {"error", m_DiagnosticRuntimeError}, {"healing", m_ResyncHealOpen}};
		if (m_LastResync.happened) {
			record["heal"] = {{"archive_bytes", m_LastResync.archiveBytes}, {"envelope_bytes", m_LastResync.envelopeBytes},
			                  {"save_ms", m_LastResync.saveMs}, {"transfer_ms", m_LastResync.transferMs}, {"heal_ms", m_LastResync.healMs}};
		}
		if (m_ResyncSavedTick.load() != UINT64_MAX) {
			record["saved_tick"] = m_ResyncSavedTick.load();
			record["boundary_tick"] = m_ResyncBoundaryTick.load();
		}
		json refusals = json::array();
		for (const ActivityMan::SaveRefusalRecord& row: g_ActivityMan.GetSaveRefusalRecords()) {
			refusals.push_back({{"kind", row.kind}, {"class", row.objectClass}, {"preset", row.presetName},
			                    {"script", row.scriptFile}, {"function", row.functionName}, {"segment", row.lastSegment},
			                    {"path", row.path}, {"player_line", row.playerLine}, {"problem", row.problem}});
		}
		record["save_refusals"] = std::move(refusals);
		return record.dump(2, ' ', false, json::error_handler_t::replace);
	}

	std::string NetMatchService::BuildReportJson() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		bool dedicated = m_Dedicated;
		int humanSeats = m_HumanSeats;
		NetMatchConfig roster = m_MatchConfig;
		if (m_Runner) {
			// Once the lobby round adopts the host's roster it, not the request, is the truth.
			const NetMatchConfig& adopted = m_Runner->GetMatchConfig();
			dedicated = adopted.dedicated;
			humanSeats = 0;
			for (const NetMatchPlayerSlot& slot : adopted.players) {
				humanSeats += slot.cpu ? 0 : 1;
			}
			roster = adopted;
		}
		json rosterPlayers = json::array();
		for (const NetMatchPlayerSlot& slot : roster.players) {
			rosterPlayers.push_back({{"peer_id", static_cast<int>(slot.peerId)}, {"team", static_cast<int>(slot.team)},
			                         {"cpu", slot.cpu}, {"display_name", slot.displayName}});
		}
		json report{
			{"pending_lobby_events", m_PendingLobbyEvents.size()},
			{"pending_lobby_overflow", m_PendingLobbyOverflow},
			{"ended_lockstep_packets", m_EndedLockstepPackets},
			{"state", StateName(m_State)},
			{"status", m_StatusText},
			{"error", m_ErrorText.empty() ? m_LobbySnapshot.errorText : m_ErrorText},
			{"activity_preset", roster.activityPreset},
			{"is_host", m_IsHost},
			{"local_peer_id", static_cast<int>(m_LocalPeerId)},
			{"local_team", m_LocalTeam},
			{"dedicated", dedicated},
			{"human_seats", humanSeats},
			// The seated roster, CPU flags and all, so a gate reads who plays from the report alone.
			{"match_config_hash", roster.players.empty() ? std::string() : NetIdentity::HashHex(NetMatchConfigUtil::HashConfig(roster))},
			{"players", std::move(rosterPlayers)},
		};
		json reconnect{
			{"admission_enabled", s_AdmissionEnabled},
			{"admission_attached", m_AdmissionAttached},
			{"ux_state", NetReconnectUx::StateName(m_ReconnectUx.GetState())},
			{"ux_attempts", m_ReconnectUx.GetAttempts()},
			{"ticket_store_path", m_TicketStore.GetPath()},
			{"ticket_stored", m_TicketStore.HasRecord()},
			{"ticket_stores", m_TicketStore.GetStores()},
			{"ticket_clears", m_TicketStore.GetClears()},
			{"ticket_refused_loads", m_TicketStore.GetRefusedLoads()},
			// Every census must come from the sim tick; anything else means a drop was seen from a
			// thread that may not walk the world, and the ledger recorded nothing for it.
			{"census_refusals", m_CensusRefusals.load()},
			// Elapsed milliseconds, so a gate can tell a real deadline from a counted pump.
			{"admission_clock_ms", AdmissionNowMs()},
			// Zero whenever setup, play and every resync read one clock; the inflation itself otherwise.
			{"clock_divergence_max_ms", m_MaxClockDivergenceMs.load()},
			{"client_state", NetReconnectClientStateName(m_ReconnectClient.GetState())},
			{"client_used_stored_ticket", m_ReconnectClient.UsedStoredTicket()},
			{"client_reclaim_outcome", m_ReconnectClient.ReclaimOutcome()},
			{"client_commits", m_ReconnectClient.GetStats().commitsReceived},
			{"client_leave_acks", m_ReconnectClient.GetStats().leaveAcksReceived},
			{"client_ambiguous_losses", m_ReconnectClient.GetStats().ambiguousLosses},
			{"client_unacknowledged_leaves", m_ReconnectClient.GetStats().unacknowledgedLeaves},
			{"client_retransmits", m_ReconnectClient.GetStats().retransmits},
			{"client_confirmed_session_ends", m_ReconnectClient.GetStats().confirmedSessionEnds},
			{"client_applications_sent", m_ReconnectClient.GetStats().applicationsSent},
			{"client_applications_acknowledged", m_ReconnectClient.GetStats().applicationsAcknowledged},
			{"client_substitution_offers", m_ReconnectClient.GetStats().substitutionOffersReceived},
			{"client_substitution_acks", m_ReconnectClient.GetStats().substitutionAcksSent},
			{"client_reject_reason", m_ReconnectClient.HasLastRejectReason() ? NetProtocol::RejectReasonName(m_ReconnectClient.GetLastRejectReason()) : ""},
			// The host's drop-and-reseat accounting, which survives the session end the seat list does
			// not: after the activity ends the seats read empty, so these are all a gate has left.
			{"host_seats_dropped", m_ReconnectHost.GetStats().seatsDropped},
			{"host_ledger_drops_recorded", m_ReconnectHost.GetStats().ledgerDropsRecorded},
			{"host_reseats_issued", m_ReconnectHost.GetStats().reseatsIssued},
			{"host_reseats_without_a_ledger", m_ReconnectHost.GetStats().reseatsWithoutALedger},
			{"host_reseats_without_survivors", m_ReconnectHost.GetStats().reseatsWithoutSurvivors},
			{"host_reseat_live_on_team_not_named", m_ReconnectHost.GetStats().reseatLiveOnTeamNotNamed},
		};
		json seats = json::array();
		for (const NetH4SeatStatus& seat: m_SeatStatuses) {
			seats.push_back({{"stable_seat", seat.stableSeat},
			                 {"lockstep_peer_id", static_cast<int>(seat.lockstepPeerId)},
			                 {"committed", seat.committed},
			                 {"closed", seat.closed},
			                 {"dropped", seat.dropped},
			                 {"reclaiming", seat.reclaiming},
			                 {"substituting", seat.substituting},
			                 {"applicants", seat.applicants}});
		}
		reconnect["seats"] = seats;
		// §9b's moderation view, so a gate can read what the host was offered and what it decided.
		json moderation = json::array();
		if (m_AdmissionAttached && m_IsHost) {
			for (const NetH4ModerationSeat& seat: m_ModerationSeats) {
				json applicants = json::array();
				for (const NetH4ApplicantView& applicant: seat.applicants) {
					applicants.push_back(json{
						{"connection", applicant.connection},
						{"display_name", applicant.displayName},
						{"approved", applicant.approved},
						{"transaction_id", applicant.transactionId},
					});
				}
				moderation.push_back(json{
					{"stable_seat", seat.stableSeat},
					{"lockstep_peer_id", static_cast<int>(seat.lockstepPeerId)},
					{"cpu", seat.cpu},
					{"display_name", seat.displayName},
					{"epoch", seat.epoch},
					{"incarnation", seat.incarnation},
					{"hold_frames_remaining", seat.holdFramesRemaining},
					{"substitution_transaction", seat.substitutionTransaction},
					{"actions_available", m_State == NetMatchServiceState::Running},
					{"committed", seat.committed},
					{"dropped", seat.dropped},
					{"closed", seat.closed},
					{"held_for_reclaim", seat.heldForReclaim},
					{"substitutable", seat.substitutable},
					{"substituting", seat.substituting},
					{"holder_generation", seat.holderGeneration},
					{"seat_generation", seat.seatGeneration},
					{"dropped_for_ms", seat.droppedForMs},
					{"applicants", applicants},
				});
			}
		}
		reconnect["moderation"] = moderation;
		// §11's roster lines exactly as this peer shows them, so a two-process gate can read a CLIENT's.
		json rosterLines = json::array();
		for (const NetLobbyMember& member: m_LobbySnapshot.members) {
			const std::string line = m_SeatPresence.Line(member.peerId, member.displayName);
			if (!line.empty()) {
				rosterLines.push_back(json{
					{"peer_id", static_cast<int>(member.peerId)},
					{"state", NetSeatPresence::StateName(m_SeatPresence.StateOf(member.peerId))},
					{"line", line},
				});
			}
		}
		reconnect["roster_lines"] = rosterLines;
		json rosterTransitions = json::array();
		for (const RosterTransition& row: m_RosterTransitions) {
			rosterTransitions.push_back(json{
				{"peer_id", static_cast<int>(row.peerId)},
				{"state", row.state},
				{"line", row.line},
				{"applied_frame", row.appliedFrame},
				{"observed_at_ms", row.observedAtMs},
			});
		}
		reconnect["roster_transitions"] = rosterTransitions;
		reconnect["roster_transitions_dropped"] = static_cast<int>(m_RosterTransitionsDropped);
		if (const auto& snapshot = m_SeatPresence.GetSnapshot()) {
			json entries = json::array();
			for (const auto& seat: snapshot->seats) {
				entries.push_back({{"stable_seat", seat.stableSeat}, {"peer_id", seat.peerId},
				                   {"state", NetSeatPresence::StateName(seat.state)}, {"holder_name", seat.holderName},
				                   {"holder_generation", seat.holderGeneration}, {"seat_generation", seat.seatGeneration},
				                   {"incarnation", seat.incarnation}, {"hold_until_frame", seat.holdUntilFrame},
				                   {"hold_active", seat.holdActive}, {"hold_until_ms", seat.holdUntilMs}});
			}
			reconnect["seat_snapshot"] = {{"epoch", snapshot->epoch}, {"session_id", snapshot->sessionId},
			                              {"round_id", snapshot->roundId}, {"revision", snapshot->revision},
			                              {"observed_at_ms", snapshot->observedAtMs}, {"seats", entries}};
		}
		if (!m_RejoinOutcome.empty()) {
			reconnect["rejoin_outcome"] = m_RejoinOutcome;
		}
		report["reconnect"] = reconnect;
		if (m_LastResync.happened) {
			report["resync"] = json{
				{"archive_bytes", m_LastResync.archiveBytes},
				{"envelope_bytes", m_LastResync.envelopeBytes},
				{"save_ms", m_LastResync.saveMs},
				{"transfer_ms", m_LastResync.transferMs},
				{"heal_ms", m_LastResync.healMs},
			};
		}
		// The snapshot's label and the tick it was taken at; a heal at a boundary has them equal.
		if (m_ResyncSavedTick.load() != UINT64_MAX) {
			report["resync"]["saved_tick"] = m_ResyncSavedTick.load();
			report["resync"]["boundary_tick"] = m_ResyncBoundaryTick.load();
		}
		// Every round's counters summed, so a resync that replaces the coordinator does not zero them.
		LockstepTotals totals = m_LockstepTotals;
		if (m_Coordinator) {
			const NetLockstepStats& live = m_Coordinator->GetStats();
			totals.peerFramesWaived += live.peerFramesWaived;
			totals.peersDroppedSilent += live.peersDroppedSilent;
			totals.connectionsClosedOnEviction += live.connectionsClosedOnEviction;
		}
		report["lockstep_totals"] = {{"peer_frames_waived", totals.peerFramesWaived},
		                             {"peers_dropped_silent", totals.peersDroppedSilent},
		                             {"connections_closed_on_eviction", totals.connectionsClosedOnEviction}};
		report["session_events"] = {{"drained_at_teardown", m_SessionEventsDrained}, {"discarded", m_SessionEventsDiscarded}};
		// The directory client's own counters; the member is game-thread only and this report is only
		// ever built there, so reading it here is safe.
		{
			json directoryReport = json::parse(m_Directory.BuildReportJson());
			if (s_PortMapRequested) {
				std::lock_guard<std::mutex> observedLock(s_ObservedIpMutex);
				directoryReport["observed_ip"] = s_DirectoryObservedIp;
			}
			report["directory"] = std::move(directoryReport);
		}
		if (s_PortMapRequested) {
			const NetPortMap::Result& mapped = s_PortMap.GetResult();
			report["port_map"] = {
				{"enabled", s_PortMapRequested},
				{"method", NetPortMap::MethodName(mapped.method)},
				{"external_ip", mapped.externalIp},
				{"external_port", mapped.externalPort},
				{"lease_s", mapped.leaseS},
				{"error", mapped.error},
			};
		}
		report["service"]["ice"] = {
			{"enabled", m_IceEnabled},
			{"join_mode", m_DirectoryRow.joinMode},
			{"identity_bound", !m_IceIdentity.empty()},
			{"bound_session_id", m_IceBoundSessionId},
			{"join_session_id", m_IceJoinSessionId},
			{"route", m_IceRoute},
		};
#ifdef CCCP_WITH_GNS
		std::string p2pReport = m_IceReport;
		if (m_Dispatcher) {
			p2pReport = m_Dispatcher->BuildReportJson();
		}
		if (!p2pReport.empty()) {
			report["p2p"] = json::parse(p2pReport, nullptr, false);
			if (m_Mux && m_Mux->P2PGns()) {
				const GnsPeerConnectionInfo info = m_Mux->P2PGns()->GetPeerConnectionInfo(1);
				report["p2p"]["connection"] = {
					{"found", info.found},
					{"state", info.state},
					{"end_reason", info.endReason},
					{"remote_identity", info.remoteIdentity},
					{"remote_address", info.remoteAddress},
					{"relayed", info.relayPop != 0},
					{"relay_pop", info.relayPop},
				};
			}
			report["p2p"]["mux"] = {{"ip_events", m_Mux ? m_Mux->IpEvents() : 0}, {"p2p_events", m_Mux ? m_Mux->P2PEvents() : 0}};
		}
#endif
		if (m_Runner && m_Session && m_Coordinator) {
			report["runner"] = json::parse(m_Runner->BuildReportJson(*m_Session, *m_Coordinator));
		} else if (!m_CapturedRunnerReport.empty()) {
			report["runner"] = json::parse(m_CapturedRunnerReport);
		} else {
			const NetReconnectHostStats& stats = m_ReconnectHost.GetStats();
			report["runner"] = json{
				{"session",
				 {{"admission", AdmissionJsonFromHost(m_ReconnectHost)},
				  {"stats", {{"fenced_disconnects", stats.fencedDisconnects}, {"fenced_packets", stats.fencedPackets}}}}},
			};
		}
		if (m_LastMatchSummary) {
			const auto& summary = *m_LastMatchSummary;
			json peers = json::array();
			for (const auto& peer : summary.peers) {
				peers.push_back({{"name", peer.name}, {"team", peer.team}, {"seat", peer.seat}, {"peer_id", peer.peerId}, {"input_delay", peer.inputDelayFrames}});
			}
			report["last_match"] = {{"result", summary.result}, {"winner_team", summary.winnerTeam}, {"running_ticks", summary.runningTicks},
			    {"duration", summary.DurationText()}, {"peers", peers}, {"resyncs", summary.resyncs}, {"drops", summary.drops},
			    {"reclaims", summary.reclaims}, {"substitutions", summary.substitutions}, {"pace", json::parse(summary.paceJson)}, {"identity", summary.IdentityText()}};
		} else {
			report["last_match"] = nullptr;
		}
		// A remote display name rides the roster and is only checked for control characters, so a
		// strict dump would throw on its first invalid byte.
		return report.dump(-1, ' ', false, json::error_handler_t::replace);
	}

	uint8_t NetMatchService::GetLocalPeerId() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_LocalPeerId;
	}

	int NetMatchService::GetLocalTeam() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_LocalTeam;
	}

	const char* NetMatchService::StateName(NetMatchServiceState state) {
		switch (state) {
			case NetMatchServiceState::Idle: return "Idle";
			case NetMatchServiceState::Starting: return "Starting";
			case NetMatchServiceState::ReadyToLaunch: return "ReadyToLaunch";
			case NetMatchServiceState::Running: return "Running";
			case NetMatchServiceState::Completed: return "Completed";
			case NetMatchServiceState::Failed: return "Failed";
		}
		return "Unknown";
	}

	bool NetMatchService::WaitForDirectorySession(uint64_t budgetMs, std::string& sessionId, std::string& token) const {
		const uint64_t deadline = SteadyNowMs() + budgetMs;
		while (SteadyNowMs() < deadline) {
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				if (m_DirectoryRegistered && !m_DirectorySessionId.empty()) {
					sessionId = m_DirectorySessionId;
					token = m_DirectoryToken;
					return true;
				}
			}
			if (m_CancelRequested.load()) {
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		return false;
	}

#ifdef CCCP_WITH_GNS
	namespace {
		GnsP2PConfig BuildIceConfig(const std::string& localIdentity, int localVirtualPort) {
			GnsP2PConfig config;
			config.stunServerList = g_SettingsMan.GetNetworkStunServers();
			// Any STUN server means reflexive candidates are wanted, so ICE runs in its default mode.
			config.iceEnable = config.stunServerList.empty() ? 2 : 0x7fffffff;
			config.localIdentity = localIdentity;
			config.localVirtualPort = localVirtualPort;
			return config;
		}

		// TURN has no per-connection config value, so the lists go on the global interface.
		void ApplyGlobalIceServers() {
			if (!SteamNetworkingUtils()) {
				return;
			}
			const auto set = [](ESteamNetworkingConfigValue value, const std::string& text) {
				SteamNetworkingUtils()->SetGlobalConfigValueString(value, text.c_str());
			};
			set(k_ESteamNetworkingConfig_P2P_STUN_ServerList, g_SettingsMan.GetNetworkStunServers());
			set(k_ESteamNetworkingConfig_P2P_TURN_ServerList, g_SettingsMan.GetNetworkTurnServers());
			set(k_ESteamNetworkingConfig_P2P_TURN_UserList, g_SettingsMan.GetNetworkTurnUser());
			set(k_ESteamNetworkingConfig_P2P_TURN_PassList, g_SettingsMan.GetNetworkTurnPass());
		}
	} // namespace
#endif

	bool NetMatchService::SetUpIceTransport(const NetMatchServiceRequest& request, const NetIdentityManifest& manifest, NetMuxTransport& mux, NetSessionConfig& sessionConfig, std::string& joinAddress, std::string* error) {
#ifndef CCCP_WITH_GNS
		(void)request; (void)manifest; (void)mux; (void)sessionConfig; (void)joinAddress;
		if (error) *error = "a session-id join needs GameNetworkingSockets";
		return false;
#else
		const std::string baseUrl = g_SettingsMan.GetSessionDirectoryUrl();
		const std::string installKey = g_SettingsMan.GetOrCreateSessionDirectoryInstallKey();
		const std::string certPin = g_SettingsMan.GetSessionDirectoryCertSha256();
		ApplyGlobalIceServers();
		m_Dispatcher = std::make_unique<GnsDirectorySignalDispatcher>();

		GnsDirectorySignalDispatcher::Config config;
		config.baseUrl = baseUrl;
		config.installKey = installKey;
		config.certPinSha256 = certPin;

		if (request.host) {
			std::string sessionId;
			std::string token;
			if (!WaitForDirectorySession(c_IceRegisterBudgetMs, sessionId, token)) {
				if (error) *error = "the session directory did not answer the register in time";
				return false;
			}
			config.role = GnsDirectorySignalDispatcher::Role::Host;
			config.sessionId = sessionId;
			config.sessionToken = token;
			if (!m_Dispatcher->Start(*mux.P2PGns(), config)) {
				if (error) *error = "the host signal channel would not open";
				return false;
			}
			const std::string iceSeed = (request.persistentWorld && !request.worldId.empty()) ? request.worldId : sessionId;
			const std::string identity = NetIceHostIdentity(iceSeed);
			mux.SetHostP2P(c_IceVirtualPort, BuildIceConfig(identity, c_IceVirtualPort));
			m_Dispatcher->SetPolling(true, SteadyNowMs());
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_IceBoundSessionId = sessionId;
				m_IceIdentity = identity;
				m_IceRoute = "ice";
			}
			std::cout << "[net-ice] host session " << sessionId << " identity " << identity << " listening on virtual port " << c_IceVirtualPort << std::endl;
			return true;
		}

		// Client: the row decides where to dial. A browse instance of its own, on this thread only.
		NetDirectoryClient browse;
		browse.Configure(baseUrl, installKey, certPin);
		NetDirectoryLocalIdentity local;
		FillDirectoryLocalIdentity(local, manifest);
		NetIdentityManifest worldManifest;
		NetDirectoryLocalIdentity worldLocal;
		std::string worldIdentityError;
		const bool worldIdentityBuilt = BuildIdentityManifest(worldManifest, &worldIdentityError, true);
		if (worldIdentityBuilt) {
			FillDirectoryLocalIdentity(worldLocal, worldManifest);
		}

		NetIceJoinTarget target;
		std::string why = "no such session";
		const uint64_t deadline = SteadyNowMs() + c_IceResolveBudgetMs;
		while (SteadyNowMs() < deadline && !m_CancelRequested.load()) {
			const uint64_t nowMs = SteadyNowMs();
			browse.PollList(nowMs);
			browse.Update(nowMs);
			if (browse.ListReplies() > 0) {
				why = NetIceResolveSessionRow(browse.Rows(), local, request.sessionId, &target, worldIdentityBuilt ? &worldLocal : nullptr);
				if (why.empty() || why != "no such session") {
					break;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		browse.StopBrowsing();
		if (!why.empty()) {
			if (error) *error = "session " + request.sessionId + ": " + why;
			return false;
		}
		if (target.persistentWorld && worldIdentityBuilt) {
			sessionConfig.localIdentity = worldManifest;
		}

		// An either/ip row that advertises an address is reached over it; ice rows have only ICE.
		if (target.joinMode != "ice" && !target.address.empty() && target.port != 0) {
			joinAddress = target.address;
			sessionConfig.port = target.port;
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_IceRoute = "ip";
			}
			std::cout << "[net-ice] session " << request.sessionId << " join_mode=" << target.joinMode << " resolved to " << target.address << ":" << target.port << "; taking the IP half" << std::endl;
			return true;
		}

		config.role = GnsDirectorySignalDispatcher::Role::Joiner;
		config.sessionId = request.sessionId;
		if (!m_Dispatcher->Start(*mux.P2PGns(), config)) {
			if (error) *error = "the joiner signal channel would not open";
			return false;
		}
		m_Dispatcher->SetPolling(true, SteadyNowMs());
		NetMuxTransport::JoinSpec spec;
		spec.peerIdentity = target.identity;
		spec.remoteVirtualPort = c_IceVirtualPort;
		spec.p2p = BuildIceConfig(std::string(), c_IceVirtualPort);
		GnsDirectorySignalDispatcher* dispatcher = m_Dispatcher.get();
		spec.makeSignaling = [dispatcher] { return dispatcher->CreateJoinSignaling(); };
		mux.SetJoinSpec(std::move(spec));
		sessionConfig.p2pJoin.identity = target.identity;
		sessionConfig.p2pJoin.remoteVirtualPort = c_IceVirtualPort;
		sessionConfig.p2pJoin.sessionId = request.sessionId;
		// The mux holds the spec, so the runner's SessionFull retry replays exactly this dial.
		sessionConfig.p2pJoin.connect = [](INetTransport& transport, std::string* connectError) { return transport.Connect(std::string(), 0, connectError); };
		joinAddress = "session:" + request.sessionId;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_IceRoute = "ice";
		}
		std::cout << "[net-ice] session " << request.sessionId << " join_mode=" << target.joinMode << " resolved to identity " << target.identity << "; dialling the ICE half" << std::endl;
		return true;
#endif
	}

	void NetMatchService::WorkerMain(NetMatchServiceRequest request, NetIdentityManifest manifest) {
		if (request.host) {
			// Arm the off-sim reconnect-auth epoch; without real crypto nothing is issued (fail closed).
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_SeatAuth.BeginHostedSession()) {
				std::cout << "[net-auth] reconnect-auth epoch armed" << std::endl;
			} else {
				std::cout << "[net-auth] crypto unavailable - reconnect auth disabled" << std::endl;
			}
		}
		auto transport = std::make_unique<GnsTransport>();
		auto session = std::make_unique<NetSession>();
		auto coordinator = std::make_unique<NetLockstepCoordinator>();
		auto runner = std::make_unique<NetMatchRunner>();
		bool iceWanted = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			iceWanted = m_IceEnabled;
			// The worker owns the session through the whole lobby; chat still needs to reach it.
			m_ChatSession = session.get();
		}
		std::unique_ptr<NetMuxTransport> mux;
		INetTransport* wire = transport.get();
		if (iceWanted) {
			mux = std::make_unique<NetMuxTransport>();
			wire = mux.get();
		}

		NetMatchRunnerConfig runnerConfig;
		runnerConfig.host = request.host;
		runnerConfig.joinAddress = request.host ? "" : request.address;
		if (!request.host) {
			const std::string sessionId = request.sessionId;
			const std::string address = request.address;
			// A browsed row is judged with this build's identity, the way the join path judges one.
			NetDirectoryLocalIdentity local;
			local.networkProtocolVersion = manifest.networkProtocolVersion;
			local.lockstepCodecVersion = manifest.deterministicConfig.lockstepCodecVersion;
			local.controllerFrameVersion = manifest.controllerFrameVersion;
			local.sessionIdentityHash = NetIdentity::HashHex(manifest.sessionIdentityHash);
			local.moduleManifestHash = NetIdentity::HashHex(manifest.moduleManifestHash);
			// The store path, the install key, the directory and the ICE choice are read here; the retry
			// itself runs on the runner's worker.
			std::string ticketPath;
			std::string installKey;
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				ticketPath = s_TicketStorePath.empty() ? NetReconnectTicketStore::DefaultPath() : s_TicketStorePath;
				installKey = g_SettingsMan.GetOrCreateSessionDirectoryInstallKey();
			}
			const std::string baseUrl = g_SettingsMan.GetSessionDirectoryUrl();
			const std::string certPin = g_SettingsMan.GetSessionDirectoryCertSha256();
			runnerConfig.resolveJoinAddress = [this, sessionId, address, iceWanted, local, ticketPath, installKey, baseUrl, certPin]() {
				NetH4TicketRecord record;
				{
					std::lock_guard<std::mutex> lock(m_Mutex);
					m_TicketStore.SetPath(ticketPath);
					if (m_TicketStore.Load(UnixNowMs(nullptr), record, nullptr) != NetH4TicketLoadResult::Loaded) {
						record = {};
					}
				}
				// A ticket left by another host is not a re-resolve of this join.
				if (!TicketMatchesRequest(record, sessionId, address)) {
					record = {};
				}
				std::vector<NetDirectorySessionRow> rows;
				const std::string id = !record.directorySessionId.empty() ? record.directorySessionId : sessionId;
				if (!id.empty() && !baseUrl.empty()) {
					NetDirectoryClient browse;
					browse.Configure(baseUrl, installKey, certPin);
					rows = BrowseSessionRows(browse, 250, [this] { return m_CancelRequested.load(); });
				}
				return ResolveTicketJoinAddressFromRows(record, sessionId, address, rows, local, iceWanted);
			};
		}
		std::string error;
		// The roster is built first: the session it is hosted on takes its seats from it.
		bool started = BuildMatchConfig(request, c_UiSessionId, runnerConfig.matchConfig, &error);
		runnerConfig.sessionConfig = BuildSessionConfig(manifest, request, runnerConfig.matchConfig);
		runnerConfig.autoInputDelay = request.autoInputDelay;
		runnerConfig.useLobbyProtocol = true;
		// Wait patiently for the other player to connect (host listening / client retrying), not the 15s default.
		runnerConfig.sessionWaitMs = c_MenuLobbyWaitMs;
		runnerConfig.lobbyWaitMs = c_MenuLobbyWaitMs;
		// First lockstep tick is 1: RestartActivity zeroes the sim count, UpdateSim increments it before MovableMan reads it.
		runnerConfig.startFrame = 1;
		// The lobby lockstep start takes the activity from the adopted roster.
		runnerConfig.autoReady = request.host;
		runnerConfig.autoStart = false;
		runnerConfig.readyRequested = &m_ReadyRequested;
		runnerConfig.startRequested = &m_StartRequested;
		runnerConfig.cancelRequested = &m_CancelRequested;
		NetMatchRunner* runnerRaw = runner.get();
		runnerConfig.publishLobby = [this, runnerRaw](const NetLobbySnapshot& snapshot) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_LobbySnapshot = snapshot;
			// The announced delay comes from the lobby's exchanged config (host-authored, already
			// auto-adjusted) — never recomputed here, so every peer renders the same value.
			const NetMatchConfig& config = runnerRaw->GetLobbySession().GetState() != NetLobbyState::Idle
			                                   ? runnerRaw->GetLobbySession().GetMatchConfig()
			                                   : runnerRaw->GetMatchConfig();
			uint8_t localPeerId = m_LocalPeerId;
			uint32_t pingMs = 0;
			for (const NetLobbyMember& member: snapshot.members) {
				if (member.isLocal) {
					localPeerId = member.peerId;
					pingMs = member.pingMs;
				}
			}
			// The readout states the host's policy, not whether a per-sender set has arrived yet: an
			// automatic delay reads automatic from the first frame and gains the measured ping later.
			const std::string measured = config.peerInputDelayFrames.empty() ? ")" : ", " + std::to_string(pingMs) + "ms ping)";
			m_InputDelayText = "Input delay: " + std::to_string(NetMatchConfigUtil::PeerInputDelay(config, localPeerId)) +
			    (config.delayPolicy == NetMatchDelayPolicy::Fixed ? " (fixed)" : " (auto" + measured);
			if (m_ChatSession) {
				// The roster's lockstep ids are the session's assigned ids plus one; the host relays
				// team scope only inside the sender's team.
				std::map<uint8_t, int> chatTeams;
				for (const NetMatchPlayerSlot& slot : config.players) {
					if (!slot.cpu && slot.peerId > 0) {
						chatTeams[static_cast<uint8_t>(slot.peerId - 1)] = slot.team;
					}
				}
				m_ChatSession->SetChatTeams(std::move(chatTeams));
			}
		};

		// One clock from here on: setup, play, stalls and every resync read the same elapsed time.
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_AdmissionClock.Start(SteadyNowMs());
		}
		runnerConfig.nowMs = [this] { return AdmissionNowMs(); };
		AttachHostPump(runnerConfig);

		// A refused roster leaves matchConfig unauthored; nothing is armed on it.
		if (started && runnerConfig.matchConfig.persistentWorld && m_WorldIdentity.IsValid()) {
			std::string worldError;
			if (!m_WorldJoin.Configure(runnerConfig.matchConfig, m_WorldIdentity, &worldError)) {
				started = false;
				error = worldError;
			}
		}
		if (started) AttachAdmissionPlane(*session, request, runnerConfig.matchConfig, runnerConfig.sessionConfig, manifest);

		if (started && iceWanted) {
			started = SetUpIceTransport(request, manifest, *mux, runnerConfig.sessionConfig, runnerConfig.joinAddress, &error);
#ifdef CCCP_WITH_GNS
			if (started && m_Dispatcher) {
				GnsDirectorySignalDispatcher* dispatcher = m_Dispatcher.get();
				mux->SetPump([dispatcher] { dispatcher->Update(SteadyNowMs()); });
			}
#endif
		}
		if (started) {
			started = runner->Start(*wire, *session, *coordinator, runnerConfig, &error);
		}
		// A joiner whose lobby round carried a match state is RECONNECTING into a live match; it
		// launches from the received snapshot instead of a fresh activity. Launching a FRESH match
		// while the others play the snapshot would desync instantly, so a failed write fails the join.
		std::string pendingLoad;
		std::optional<NetResyncState> pendingState;
		if (started) {
			std::vector<uint8_t> receivedState = runner->TakeReceivedState();
			if (!receivedState.empty()) {
				if (IsWorldJoinImageBlob(receivedState)) {
					started = PrepareReceivedWorldJoin(receivedState, pendingLoad, &error);
				} else {
					NetResyncState state;
					started = PrepareReceivedResync(receivedState, *coordinator, pendingLoad, state, &error);
					if (started) pendingState = std::move(state);
				}
			}
		}
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (started) {
				// The lockstep peer id is the session-assigned id + 1; the team comes from that slot.
				const uint8_t localLockstepId = static_cast<uint8_t>(session->GetLocalPeerId() + 1);
				int localTeam = Activity::NoTeam;
				for (const NetMatchPlayerSlot& slot : runner->GetMatchConfig().players) {
					if (slot.peerId == localLockstepId) {
						localTeam = slot.team;
						break;
					}
				}
				m_Mux = std::move(mux);
				m_Transport = std::move(transport);
				m_Session = std::move(session);
				m_Coordinator = std::move(coordinator);
				m_Runner = std::move(runner);
				m_LocalPeerId = localLockstepId;
				m_LocalTeam = localTeam;
				m_PendingResyncLoad = pendingLoad;
				m_PendingResyncState = std::move(pendingState);
				m_State = NetMatchServiceState::ReadyToLaunch;
				m_StatusText = "Ready to launch match";
				m_ErrorText.clear();
			} else {
				// Keep the objects on failure too — the report needs the session's reject record.
				m_Mux = std::move(mux);
				m_Transport = std::move(transport);
				m_Session = std::move(session);
				m_Coordinator = std::move(coordinator);
				m_Runner = std::move(runner);
				m_State = NetMatchServiceState::Failed;
				m_StatusText = (m_Session && m_Session->HasReject() && m_Session->GetRejectSummary() == "Match roster refused")
					? "Match roster refused" : "Network setup failed";
				m_ErrorText = error;
				// §9b: a live match is the one refusal a joiner can answer, by applying for a seat.
				m_JoinRefusedByLiveMatch = !request.host && m_Session && m_Session->HasReject() &&
				                           m_Session->GetMismatchKey() == "live_match";
			}
			m_WorkerDone = true;
		}
	}

	bool NetMatchService::WaitForA7ConnectGate(std::string* error) {
		if (!NetA7Journal::HasConnectGate()) return true;
		NetReconnectTicketStore store;
		store.SetPath(s_TicketStorePath.empty() ? NetReconnectTicketStore::DefaultPath() : s_TicketStorePath);
		NetH4TicketRecord record;
		const auto result = store.Load(UnixNowMs(nullptr), record, nullptr);
		NetA7Journal::Emit("ticket_preload", {{"load_result", static_cast<int>(result)}, {"ticket_sha256", store.GetA7LoadedSha256()}});
		return NetA7Journal::WaitForConnectGate(store.GetA7LoadedSha256(), error);
	}

	bool NetMatchService::CanSealA7Journal() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return !m_Worker.joinable() || m_WorkerDone;
	}

	void NetMatchService::CaptureA7SeatView() {
		if (!NetA7Journal::Enabled()) return;
		json rows = json::array();
		if (m_IsHost && m_AdmissionAttached) {
			for (const auto& seat: m_ReconnectHost.GetSeatStatuses()) {
				rows.push_back({{"stable_seat", seat.stableSeat}, {"committed", seat.committed}, {"dropped", seat.dropped}, {"closed", seat.closed}});
			}
		}
		NetA7Journal::SetSeatView(std::move(rows), AdmissionNowMs(), m_IsHost ? "host_admission_plane" : "host_rows_unavailable_on_client");
	}

	void NetMatchService::SetTicketStorePath(std::string path) {
		s_TicketStorePath = std::move(path);
	}

	void NetMatchService::SetJoinWaitPath(std::string path) {
		s_JoinWaitPath = std::move(path);
	}

	bool NetMatchService::WaitForJoinTrigger(const std::string& path, uint64_t budgetMs, std::string* error) {
		if (path.empty()) return true;
		const std::filesystem::path trigger(path);
		const auto opened = std::chrono::steady_clock::now();
		std::cout << "[net-join-wait] waiting for " << path << std::endl;
		for (;;) {
			std::error_code fsError;
			if (std::filesystem::exists(trigger, fsError) && !fsError) {
				const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - opened).count();
				std::cout << "[net-join-wait] released after " << waitedMs << "ms" << std::endl;
				return true;
			}
			const uint64_t elapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - opened).count());
			if (elapsedMs >= budgetMs) break;
			const uint64_t remainingMs = budgetMs - elapsedMs;
			std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint64_t>(c_JoinWaitPollMs, remainingMs)));
		}
		if (error) *error = "join wait timed out: " + path + " did not appear within " + std::to_string(budgetMs) + "ms";
		std::cerr << "[net-join-wait] timed out after " << budgetMs << "ms waiting for " << path << std::endl;
		return false;
	}

	void NetMatchService::SetApplyForSeat(bool enabled, uint16_t stableSeat) {
		s_ApplyForSeat = enabled;
		s_ApplySeat = stableSeat;
	}

	void NetMatchService::SetAutoSubstitute(bool enabled, uint16_t stableSeat, uint64_t delayMs, bool thenCancel) {
		s_AutoSubstitute = enabled;
		s_AutoSubstituteSeat = stableSeat;
		s_AutoSubstituteDelayMs = delayMs;
		s_AutoSubstituteThenCancel = thenCancel;
	}

	std::vector<NetH4ModerationSeat> NetMatchService::GetModerationSeats() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_AdmissionAttached || !m_IsHost) {
			return {};
		}
		auto seats = m_ModerationSeats;
		for (auto& seat: seats) seat.actionsAvailable = m_State == NetMatchServiceState::Running;
		return seats;
	}

	NetH4ModerationResult NetMatchService::ApplyModeration(const NetModerationSelection& selection, NetModerationAction action) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_AdmissionAttached || !m_IsHost) {
			return NetH4ModerationResult::NotHosting;
		}
		if (m_State != NetMatchServiceState::Running || !m_Session || !m_Coordinator) {
			return NetH4ModerationResult::ActionUnavailable;
		}
		const uint64_t nowMs = AdmissionNowMs();
		const NetH4ModerationResult result = m_ReconnectHost.ApplyModeration(selection, action, nowMs);
		if (result == NetH4ModerationResult::Ok) {
			RecordModerationAction(selection.stableSeat, action);
		}
		m_Session->TickAdmissionPlane(nowMs);
		PublishModerationView();
		return result;
	}

	NetKickBanResult NetMatchService::ApplyRemovalLocked(const NetModerationSelection& selection, NetParticipantRemovalAction action, NetSession& session) {
		const uint64_t nowMs = AdmissionNowMs();
		const uint64_t sessionId = session.GetSessionId();
		// Between rounds the coordinator is gone and both peers still hold the round they played, so
		// that is what the notice is stamped with; a fresh stamp of 0 would read as stale on the client.
		const uint32_t round = m_Coordinator ? static_cast<uint32_t>(m_Coordinator->GetRoundId()) : m_LastRoundId;
		const uint64_t boundary = m_Coordinator ? m_Coordinator->GetStats().nextFrame : 0;
		m_LastKickBanResult = m_ReconnectHost.RemoveParticipant(selection, action, nowMs, UnixNowMs(nullptr), sessionId, round, boundary, m_LastRemovalIssue);
		if (m_LastKickBanResult != NetKickBanResult::Ok) {
			return m_LastKickBanResult;
		}
		session.BroadcastControl(m_LastRemovalIssue.notice);
		if (m_Coordinator && m_State == NetMatchServiceState::Running && m_LastRemovalIssue.lockstepPeerId != 0) {
			const char* why = action == NetParticipantRemovalAction::Kick ? "removed from the session" : "banned from the session";
			m_Coordinator->EvictRemovedPeer(m_LastRemovalIssue.lockstepPeerId, why, nowMs);
		}
		if (m_LastRemovalIssue.connection != c_InvalidNetPeerId) {
			const NetRejectReason reason = action == NetParticipantRemovalAction::Kick ? NetRejectReason::ParticipantRemoved : NetRejectReason::ParticipantBanned;
			const char* text = action == NetParticipantRemovalAction::Kick ? "removed from this session" : "banned from this session";
			session.DisconnectReadyPeer(m_LastRemovalIssue.connection, reason, text);
		}
		session.TickAdmissionPlane(nowMs);
		if (m_State == NetMatchServiceState::Running) {
			PublishModerationView();
		}
		const std::string who = m_LocalName.empty() ? "Host" : m_LocalName;
		// A Starting kick runs on the setup worker, and the toast queue is the game thread's, so the
		// line waits for the next pump instead of being pushed from here.
		m_PendingToasts.push_back(who + std::string(action == NetParticipantRemovalAction::Kick ? " removed " : " banned ") + "seat " + std::to_string(selection.stableSeat));
		return m_LastKickBanResult;
	}

	void NetMatchService::PushPendingToasts() {
		std::vector<std::string> toasts;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			toasts.swap(m_PendingToasts);
		}
		for (const std::string& line : toasts) {
			ScenarioRunner::PushNetUiToast("moderation", line);
		}
	}

	NetKickBanResult NetMatchService::ApplyUnbanLocked(const NetAuthBytes32& identity) {
		std::string error;
		m_LastKickBanResult = m_BanStore.Unban(identity, &error) ? NetKickBanResult::Ok : NetKickBanResult::PersistenceFailed;
		return m_LastKickBanResult;
	}

	NetKickBanResult NetMatchService::QueueModerationLocked(const PendingModeration& pending) {
		// A queue no lobby could fill is a flood, and it is refused rather than grown.
		constexpr size_t c_MaxPendingModeration = 16;
		if (m_PendingModeration.size() >= c_MaxPendingModeration) {
			m_LastKickBanResult = NetKickBanResult::ActionUnavailable;
			return m_LastKickBanResult;
		}
		m_PendingModeration.push_back(pending);
		m_LastKickBanResult = NetKickBanResult::Queued;
		return m_LastKickBanResult;
	}

	void NetMatchService::AttachHostPump(NetMatchRunnerConfig& config) {
		config.pumpHost = [this](NetSession& session) { DrainPendingModeration(session); };
	}

	void NetMatchService::DrainPendingModeration(NetSession& session) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_PendingModeration.empty() || m_State != NetMatchServiceState::Starting) {
			return;
		}
		std::vector<PendingModeration> pending;
		pending.swap(m_PendingModeration);
		NetKickBanResult issue = NetKickBanResult::Ok;
		for (const PendingModeration& action : pending) {
			// In the order the host asked for them: a ban queued after an unban of the same identity
			// must still leave that identity banned.
			const NetKickBanResult result = action.unban ? ApplyUnbanLocked(action.identity)
			                                             : ApplyRemovalLocked(action.selection, action.action, session);
			if (issue == NetKickBanResult::Ok) {
				issue = result;
			}
		}
		// The host is told about the first action that was refused, not the last one that worked.
		m_LastKickBanResult = issue;
	}

	NetKickBanResult NetMatchService::RemoveParticipant(const NetModerationSelection& selection, NetParticipantRemovalAction action) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_LastRemovalIssue = {};
		if (!m_AdmissionAttached || !m_IsHost) {
			m_LastKickBanResult = NetKickBanResult::NotHosting;
			return m_LastKickBanResult;
		}
		if (m_State != NetMatchServiceState::Running && m_State != NetMatchServiceState::Starting) {
			m_LastKickBanResult = NetKickBanResult::ActionUnavailable;
			return m_LastKickBanResult;
		}
		if (m_State == NetMatchServiceState::Starting) {
			// The setup worker owns the session for the whole of Start, so the kick is applied there and
			// the result is not known yet.
			return QueueModerationLocked(PendingModeration{false, selection, action, {}});
		}
		if (!m_Session) {
			m_LastKickBanResult = NetKickBanResult::ActionUnavailable;
			return m_LastKickBanResult;
		}
		return ApplyRemovalLocked(selection, action, *m_Session);
	}

	NetKickBanResult NetMatchService::GetLastKickBanResult() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_LastKickBanResult;
	}

	NetParticipantRemovalIssue NetMatchService::GetLastRemovalIssue() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_LastRemovalIssue;
	}

	NetKickBanResult NetMatchService::UnbanParticipant(const NetAuthBytes32& identity) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_IsHost) {
			m_LastKickBanResult = NetKickBanResult::NotHosting;
			return m_LastKickBanResult;
		}
		if (m_State == NetMatchServiceState::Starting) {
			// The setup worker owns admission for the whole of Start, so the store is written there and
			// never from this thread; the unban keeps its place among the queued kicks.
			return QueueModerationLocked(PendingModeration{true, {}, NetParticipantRemovalAction::Kick, identity});
		}
		return ApplyUnbanLocked(identity);
	}

	std::vector<NetHostBanRecord> NetMatchService::GetBanRecords() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_BanStore.List();
	}

	void NetMatchService::RecordModerationAction(uint16_t stableSeat, NetModerationAction action) {
		const std::string who = m_LocalName.empty() ? "Host" : m_LocalName;
		const std::string seat = " for seat " + std::to_string(stableSeat);
		const char* verb = action == NetModerationAction::Wait ? " waits for the player" :
		                   action == NetModerationAction::Substitute ? " approved a substitute" : " cancelled substitution";
		ScenarioRunner::PushNetUiToast("moderation", who + verb + seat);
	}

	void NetMatchService::DriveAutoSubstitution(uint64_t nowMs) {
		if (!s_AutoSubstitute || m_AutoSubstituteDone) {
			return;
		}
		for (const NetH4ModerationSeat& seat : m_ReconnectHost.GetModerationView()) {
			if (seat.stableSeat != s_AutoSubstituteSeat || !seat.substitutable || seat.applicants.empty()) {
				continue;
			}
			if (m_AutoSubstituteReadyMs == 0) {
				m_AutoSubstituteReadyMs = nowMs;
			}
			if (nowMs < m_AutoSubstituteReadyMs + s_AutoSubstituteDelayMs) {
				return;
			}
			const NetH4ModerationResult result = m_ReconnectHost.SubstituteApplicant(seat.stableSeat, seat.applicants.front().connection, nowMs);
			if (result == NetH4ModerationResult::Ok) {
				RecordModerationAction(seat.stableSeat, NetModerationAction::Substitute);
			}
			std::cout << "[net-reconnect] moderation: substitute seat " << seat.stableSeat << " -> "
			          << NetH4ModerationResultName(result) << std::endl;
			if (result == NetH4ModerationResult::Ok && s_AutoSubstituteThenCancel) {
				const NetH4ModerationResult cancelled = m_ReconnectHost.CancelSubstitution(seat.stableSeat, nowMs);
				std::cout << "[net-reconnect] moderation: cancel seat " << seat.stableSeat << " -> "
				          << NetH4ModerationResultName(cancelled) << std::endl;
				if (cancelled == NetH4ModerationResult::Ok) {
					RecordModerationAction(seat.stableSeat, NetModerationAction::Cancel);
				}
			}
			m_AutoSubstituteDone = true;
			return;
		}
	}


	std::vector<NetH4LedgerActor> NetMatchService::CollectDropOwnership(void* context) {
		auto* service = static_cast<NetMatchService*>(context);
		if (!t_SimCensusOpen) {
			// A drop seen from the setup worker, not the sim tick. Walking g_MovableMan from there is a
			// data race, so the seat records an empty ledger rather than a torn one.
			if (service) {
				service->m_CensusRefusals.fetch_add(1);
			}
			return {};
		}
		std::vector<NetH4LedgerActor> actors;
		for (const MovableMan::LockstepActorOwner& owner: g_MovableMan.BuildLockstepOwnershipCensus()) {
			actors.push_back({owner.actorUID, owner.team, owner.ownerPeerId, true});
		}
		return actors;
	}

	NetLockstepSeatState NetMatchService::QuerySeatState(void* context, uint8_t lockstepPeerId, NetPeerId transportPeerId) {
		auto* service = static_cast<NetMatchService*>(context);
		NetLockstepSeatState state;
		if (!service) {
			return state;
		}
		std::lock_guard<std::mutex> lock(service->m_Mutex);
		if (!service->m_AdmissionAttached || !service->m_IsHost) {
			return state;
		}
		state.fencedTransport = transportPeerId != c_InvalidNetPeerId && service->m_ReconnectHost.IsFenced(transportPeerId);
		state.heldForReclaim = service->m_ReconnectHost.IsSeatHeldForReclaim(lockstepPeerId);
		return state;
	}

	void NetMatchService::AttachAdmissionPlane(NetSession& session, const NetMatchServiceRequest& request, const NetMatchConfig& matchConfig, const NetSessionConfig& sessionConfig, const NetIdentityManifest& manifest) {
		m_AdmissionAttached = false;
		if (!s_AdmissionEnabled) {
			return;
		}
		const NetH4Identity identity = BuildH4Identity(manifest);
		if (request.host) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (!m_SeatAuth.IsActive()) {
				// Fail closed: with no crypto nothing can be issued or proven, so the session keeps the
				// pre-admission handshake rather than gating every join on a ticket it cannot mint.
				return;
			}
			m_ReconnectHost.Configure(&m_SeatAuth, sessionConfig.sessionId, identity);
			m_ReconnectHost.SetSeatTable(NetH4BuildSeatTable(matchConfig), matchConfig.mode);
			m_ReconnectHost.SetHostAddress(NetLanDiscovery::GetPrimaryLocalAddress());
			m_ReconnectHost.SetMatchConfigHash(NetMatchConfigUtil::HashConfig(matchConfig));
			m_ReconnectHost.SetLiveMatch(false);
			m_ReconnectHost.SetPersistentWorld(matchConfig.persistentWorld);
			m_ReconnectHost.SetDropOwnershipSource(&NetMatchService::CollectDropOwnership, this);
			session.SetReconnectHost(&m_ReconnectHost);
			session.EnableParticipantProof(nullptr);
			m_BanStore.SetPath(NetHostBanStore::DefaultPath());
			if (!m_BanStore.Load(nullptr)) {
				// Last-good persistents stay; Until Removed writes stay closed until a later load.
			}
			m_ReconnectHost.SetBanStore(&m_BanStore);
			session.SetHostBanStore(&m_BanStore);
			m_AdmissionAttached = true;
			return;
		}
		m_TicketStore.SetPath(s_TicketStorePath.empty() ? NetReconnectTicketStore::DefaultPath() : s_TicketStorePath);
		m_ParticipantStore.SetPath(NetParticipantIdentityStore::DefaultPath());
		(void)m_ParticipantStore.LoadOrCreate(nullptr);
		m_ReconnectClient.Configure(&m_TicketStore, identity, request.playerName.empty() ? "Client" : request.playerName);
		m_ReconnectClient.SetUnixClock(&UnixNowMs, nullptr);
		// The record names the host it belongs to; the config hash is context, not a gate - a client
		// adopts the host's match config in the lobby round that follows.
		m_ReconnectClient.SetHostContext(request.address, NetHash32{});
		m_ReconnectClient.SetWorldTarget(request.persistentWorld || matchConfig.persistentWorld);
		m_ReconnectClient.SetDirectorySessionId(request.sessionId);
		m_ReconnectClient.SetApplyForSeat(s_ApplyForSeat || s_ApplyOnce, s_ApplyOnce ? c_NetH4AnySubstitutableSeat : s_ApplySeat);
		s_ApplyOnce = false;
		session.SetReconnectClient(&m_ReconnectClient);
		session.EnableParticipantProof(&m_ParticipantStore);
		m_AdmissionAttached = true;
	}

	void NetMatchService::ScanStoredTicket() {
		if (!s_AdmissionEnabled) {
			m_ReconnectUx.DismissOffer();
			return;
		}
		m_TicketStore.SetPath(s_TicketStorePath.empty() ? NetReconnectTicketStore::DefaultPath() : s_TicketStorePath);
		NetH4TicketRecord record;
		const NetH4TicketLoadResult load = m_TicketStore.Load(UnixNowMs(nullptr), record, nullptr);
		m_ReconnectUx.OfferStoredTicket(load, record.hostAddress);
	}

	NetMatchServiceRequest NetMatchService::BuildTicketRejoinRequest(const NetH4TicketRecord& record, const std::string& playerName, bool liveWorldTarget) {
		NetMatchServiceRequest request;
		request.host = false;
		request.address = record.hostAddress;
		request.playerName = playerName.empty() ? "Client" : playerName;
		request.resyncOnDesync = true;
		// The ticket's own flag survives a relaunch, which the live flags do not.
		request.persistentWorld = record.persistentWorld || liveWorldTarget;
		if (request.persistentWorld) {
			request.activityPreset = "Persistent World";
		}
		return request;
	}

	bool NetMatchService::BeginTicketRejoin(std::string* error) {
		NetH4TicketRecord record;
		m_TicketStore.SetPath(s_TicketStorePath.empty() ? NetReconnectTicketStore::DefaultPath() : s_TicketStorePath);
		const NetH4TicketLoadResult load = m_TicketStore.Load(UnixNowMs(nullptr), record, nullptr);
		m_ReconnectUx.OfferStoredTicket(load, record.hostAddress);
		if (load != NetH4TicketLoadResult::Loaded) {
			if (error) *error = m_ReconnectUx.GetOfferText().empty() ? "no reconnect ticket to rejoin with" : m_ReconnectUx.GetOfferText();
			return false;
		}
		return Start(BuildTicketRejoinRequest(record, m_LocalName, m_MatchConfig.persistentWorld || m_LastJoinTargetPersistentWorld), error);
	}

	NetSessionConfig NetMatchService::BuildSessionConfig(const NetIdentityManifest& manifest, const NetMatchServiceRequest& request, const NetMatchConfig& matchConfig) const {
		NetSessionConfig config;
		config.localIdentity = manifest;
		config.displayName = request.playerName.empty() ? (request.host ? "Host" : "Client") : request.playerName;
		config.port = request.port;
		config.sessionId = c_UiSessionId;
		config.localNonce = request.host ? c_HostNonce : MakeClientNonce();
		// Seats come from the adopted roster, never from the request: a round the host plays alone offers none.
		config.maxPeers = matchConfig.persistentWorld
			? NetMatchConfigUtil::c_MaxPeerCount
			: static_cast<uint8_t>(std::max(0, static_cast<int>(matchConfig.peerCount) - 1));
		config.readyWithoutPeers = request.host && matchConfig.persistentWorld;
		config.heartbeatIntervalMs = 50;
		config.timeoutMs = 5000;
		config.rejectUserdataModules = false;
		return config;
	}

	bool NetMatchService::BuildMatchConfig(const NetMatchServiceRequest& request, uint64_t sessionId, NetMatchConfig& outConfig, std::string* error) {
		auto refuse = [&](const char* reason) { if (error) *error = reason; return false; };
		if (request.peerCount < NetMatchConfigUtil::c_MinPeerCount || request.peerCount > NetMatchConfigUtil::c_MaxPeerCount) return refuse("peer_count is out of range");
		const uint32_t capacity = request.peerCount - (request.dedicated ? 1 : 0);
		const uint32_t humanCount = request.humans.value_or(capacity);
		const NetMatchMode mode = request.mode;
		const uint32_t cpuCount = request.cpuSlots.value_or(mode == NetMatchMode::PvPSkirmish ? 0 : 1);
		if (humanCount > capacity) return refuse("human seats exceed peer capacity");
		if (humanCount == 0 && !request.dedicated) return refuse("zero human seats require a dedicated host");
		if (mode == NetMatchMode::PvPvE && humanCount == Activity::Teams::MaxTeamCount) return refuse("four-human PvPvE exceeds team capacity");
		const uint32_t firstCPUTeam = mode == NetMatchMode::CoopPvE ? 1 : humanCount;
		if (cpuCount > Activity::Teams::MaxTeamCount || firstCPUTeam + cpuCount > Activity::Teams::MaxTeamCount) return refuse("CPU seats exceed team capacity");
		// Co-op seats every human on one team, so a full lobby can ask for more slots than the wire carries.
		if (humanCount + cpuCount > NetMatchConfigUtil::c_MaxPlayers) return refuse("roster exceeds the player slot capacity");
		NetMatchConfig config = NetMatchConfigUtil::MakeDefault(sessionId);
		config.activityPreset = request.activityPreset.empty() ? "P4 Alpha Duel" : request.activityPreset;
		// A named module rides the request; an unnamed one keeps the default the launch config carries.
		if (!request.activityModule.empty()) {
			config.activityModule = request.activityModule;
		}
		if (request.standardRules) {
			static_cast<NetMatchStandardRules&>(config) = *request.standardRules;
		}
		// A named class seats CreateConfiguredActivity; empty keeps MakeDefault's GAScripted.
		if (!request.activityType.empty()) {
			config.activityType = request.activityType;
		}
		if (!request.sceneName.empty()) {
			config.sceneName = request.sceneName;
			if (!request.sceneModule.empty()) {
				config.sceneModule = request.sceneModule;
			}
		}
		config.mode = mode;
		config.modePreset = NetMatchConfigUtil::ModeName(mode);
		config.ownershipPolicy = request.ownershipPolicy;
		config.inputDelayFrames = request.inputDelayFrames;
		// The rule the request carries seats the round; SeatSavedOptions put the host's Gameplay setting there.
		config.brainlessHumansSpectate = request.brainlessHumansSpectate.value_or(config.brainlessHumansSpectate);
		const bool world = request.host && (request.persistentWorld || request.activityPreset == "Persistent World");
		const uint32_t seatedHumans = world ? request.humans.value_or(0) : humanCount;
		if (world) {
			if (!request.dedicated) return refuse("a persistent world is hosted by a dedicated host");
			config.persistentWorld = true;
			config.version = NetMatchConfigUtil::c_PersistentWorldVersion;
			config.worldId = request.worldId;
			config.worldBoot = request.worldBoot;
			config.activityPreset = request.activityPreset.empty() ? "Persistent World" : request.activityPreset;
			config.peerCount = request.peerCount;
		} else {
			config.peerCount = humanCount == 0 ? 1 : request.peerCount;
		}
		config.dedicated = request.dedicated;
		// The host publishes the checkpoint cadence the whole match follows; a client's own setting never steers one.
		if (request.host) {
			const uint32_t seconds = std::min(GetAutosaveSeconds(), c_MaxAutosaveIntervalSeconds);
			config.autosaveEnabled = seconds > 0;
			config.autosaveIntervalSeconds = seconds;
			// The rest of the host's saved session options ride the same config to every peer.
			config.delayPolicy = request.delayPolicy.value_or(config.delayPolicy);
			config.idleWaitMinutes = request.idleWaitMinutes.value_or(config.idleWaitMinutes);
			config.automaticRepair = request.automaticRepair.value_or(config.automaticRepair);
			config.pathHorizonTicks = request.pathHorizonTicks.value_or(config.pathHorizonTicks);
		}
		// CPU teams follow human teams and consume no peer identity.
		config.players.clear();
		const uint8_t firstHumanPeer = request.dedicated ? 2 : 1;
		const uint32_t rosterHumans = world ? seatedHumans : humanCount;
		for (uint8_t peerId = firstHumanPeer; peerId < firstHumanPeer + rosterHumans; ++peerId) {
			NetMatchPlayerSlot slot;
			slot.peerId = peerId;
			slot.team = mode == NetMatchMode::CoopPvE ? 0 : static_cast<uint8_t>(peerId - firstHumanPeer);
			slot.cpu = false;
			slot.displayName = peerId == config.hostPeerId ? PlayerNameOrDefault(request, true)
			                                               : ("Client " + std::to_string(peerId));
			config.players.push_back(slot);
		}
		for (uint32_t cpu = 0; cpu < cpuCount; ++cpu) {
			NetMatchPlayerSlot cpuSlot;
			cpuSlot.peerId = 0;
			cpuSlot.team = static_cast<uint8_t>(firstCPUTeam + cpu);
			cpuSlot.cpu = true;
			cpuSlot.displayName = cpuCount == 1 ? "CPU" : "CPU " + std::to_string(cpu + 1);
			config.players.push_back(cpuSlot);
		}
		if (world && rosterHumans == 0) {
			for (uint8_t peerId = firstHumanPeer; peerId <= config.peerCount; ++peerId) {
				NetMatchPlayerSlot slot;
				slot.peerId = peerId;
				slot.team = static_cast<uint8_t>(peerId - firstHumanPeer);
				slot.cpu = false;
				slot.displayName = "Open";
				config.players.push_back(slot);
			}
		}
		if (!NetMatchConfigUtil::ValidateLocalAlpha(config, error)) return false;
		outConfig = std::move(config);
		return true;
	}

	bool NetMatchService::ResolveActivityModule(const std::string& preset, const std::vector<std::string>& definingModules, std::string& outModule, std::string* error) {
		if (definingModules.size() > 1) {
			// Never pick one silently: the caller has to say which module it means.
			std::string named;
			for (const std::string& module: definingModules) {
				named += named.empty() ? module : ", " + module;
			}
			if (error) *error = "match activity " + preset + " is defined by " + named + "; name its module";
			return false;
		}
		// No definition leaves the module unset, so the launch refuses by the name the config carries.
		outModule = definingModules.empty() ? "" : definingModules.front();
		return true;
	}

	namespace {
		std::vector<Scene*> CollectHostScenes() {
			std::list<Entity*> presets;
			g_PresetMan.GetAllOfType(presets, "Scene");
			std::vector<Scene*> scenes;
			for (Entity* entity: presets) {
				Scene* scene = dynamic_cast<Scene*>(entity);
				if (scene && !scene->GetLocation().IsZero() && !scene->IsMetagameInternal() && !scene->IsSavedGameInternal() &&
				    (scene->GetMetasceneParent().empty() || g_SettingsMan.ShowMetascenes())) {
					scenes.push_back(scene);
				}
			}
			return scenes;
		}

		GameActivity* FindHostActivity(const std::string& preset, const std::string& module) {
			std::list<Entity*> presets;
			g_PresetMan.GetAllOfType(presets, "Activity");
			for (Entity* entity: presets) {
				auto* activity = dynamic_cast<GameActivity*>(entity);
				if (!activity || activity->GetPresetName() != preset) {
					continue;
				}
				const std::string defined = g_PresetMan.GetDataModuleName(activity->GetModuleID());
				if (module.empty() || defined == module) {
					return activity;
				}
			}
			return nullptr;
		}

		std::vector<NetHostSceneChoice> ScenesForActivity(GameActivity* activity, const std::vector<Scene*>& scenes) {
			std::vector<NetHostSceneChoice> out;
			if (!activity) {
				return out;
			}
			for (Scene* scene: scenes) {
				if (activity->SceneIsCompatible(scene)) {
					out.push_back({scene->GetPresetName(), g_PresetMan.GetDataModuleName(scene->GetModuleID())});
				}
			}
			return out;
		}
	}

	std::vector<NetHostActivityChoice> NetMatchService::ListLoadedGameActivities() {
		const std::vector<Scene*> scenes = CollectHostScenes();
		std::list<Entity*> presets;
		g_PresetMan.GetAllOfType(presets, "Activity");
		std::vector<NetHostActivityChoice> out;
		for (Entity* entity: presets) {
			auto* activity = dynamic_cast<GameActivity*>(entity);
			if (!activity || activity->IsTestActivity()) {
				continue;
			}
			NetHostActivityChoice row;
			row.preset = activity->GetPresetName();
			row.module = g_PresetMan.GetDataModuleName(activity->GetModuleID());
			row.activityType = activity->GetClassName();
			row.scenes = ScenesForActivity(activity, scenes);
			out.push_back(std::move(row));
		}
		return out;
	}

	std::vector<NetHostActivityChoice> NetMatchService::ListHostActivities() {
		std::vector<NetHostActivityChoice> out;
		for (NetHostActivityChoice& row: ListLoadedGameActivities()) {
			if (!row.scenes.empty()) {
				out.push_back(std::move(row));
			}
		}
		return out;
	}

	std::vector<NetHostSceneChoice> NetMatchService::ListHostScenes(const std::string& preset, const std::string& module) {
		return ScenesForActivity(FindHostActivity(preset, module), CollectHostScenes());
	}

	bool NetMatchService::ResolveHostScene(const std::string& preset, const std::string& module, std::string& sceneName, std::string& sceneModule) {
		const std::vector<NetHostSceneChoice> scenes = ListHostScenes(preset, module);
		if (scenes.empty()) {
			return false;
		}
		for (const NetHostSceneChoice& scene: scenes) {
			if (scene.name == "Grasslands") {
				sceneName = scene.name;
				sceneModule = scene.module;
				return true;
			}
		}
		sceneName = scenes.front().name;
		sceneModule = scenes.front().module;
		return true;
	}

	bool NetMatchService::SeatHostScene(NetMatchServiceRequest& request, std::string* error) {
		if (!request.sceneName.empty()) {
			return true;
		}
		const std::string preset = request.activityPreset.empty() ? "P4 Alpha Duel" : request.activityPreset;
		if (!ResolveHostScene(preset, request.activityModule, request.sceneName, request.sceneModule)) {
			if (error) *error = "match activity " + preset + " has no compatible scene";
			return false;
		}
		return true;
	}

	void NetMatchService::ApplyHostActivityFallback(NetMatchServiceRequest& request) {
		if (request.activityPreset.empty()) {
			request.activityPreset = "P4 Alpha Duel";
		}
		if (request.activityModule.empty()) {
			request.activityModule = "Base.rte";
		}
	}

	bool NetMatchService::SeatActivityModule(NetMatchServiceRequest& request, std::string* error) {
		if (!request.activityModule.empty()) {
			return true;
		}
		const NetMatchStandardRules defaults;
		const std::string preset = request.activityPreset.empty() ? defaults.activityPreset : request.activityPreset;
		std::vector<std::string> definingModules;
		for (int moduleId = 0; moduleId < g_PresetMan.GetTotalModuleCount(); ++moduleId) {
			// GetEntityPreset falls back to the official modules, so only a module's own definition counts.
			const Entity* found = g_PresetMan.GetEntityPreset(defaults.activityType, preset, moduleId);
			if (found && found->GetModuleID() == moduleId) {
				definingModules.push_back(g_PresetMan.GetDataModuleName(moduleId));
			}
		}
		return ResolveActivityModule(preset, definingModules, request.activityModule, error);
	}

	void NetMatchService::SeatSavedOptions(NetMatchServiceRequest& request) {
		if (!request.brainlessHumansSpectate) request.brainlessHumansSpectate = g_SettingsMan.GetBrainlessHumansSpectate();
		if (!request.host) return;
		// The saved-to-wire mapping lives with the config, so the values are read through it.
		NetMatchConfig saved;
		NetMatchConfigUtil::ApplySavedHostOptions(saved);
		if (!request.delayPolicy) request.delayPolicy = saved.delayPolicy;
		if (!request.idleWaitMinutes) request.idleWaitMinutes = saved.idleWaitMinutes;
		if (!request.automaticRepair) request.automaticRepair = saved.automaticRepair;
		if (!request.pathHorizonTicks) request.pathHorizonTicks = saved.pathHorizonTicks;
	}

	void NetMatchService::SetState(NetMatchServiceState state, std::string status, std::string error) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_State = state;
		m_StatusText = std::move(status);
		m_ErrorText = std::move(error);
	}

	void NetMatchService::JoinWorkerIfDone() {
		bool done = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			done = m_WorkerDone;
		}
		if (done && m_Worker.joinable()) {
			m_Worker.join();
		}
	}

} // namespace RTE
