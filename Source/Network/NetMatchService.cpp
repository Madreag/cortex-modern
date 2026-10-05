#include "NetMatchService.h"
#include "LoopbackTransport.h"
#include "NetA7Journal.h"
#include "NetAuthCrypto.h"

#include "ActivityMan.h"
#include "MetricsCollector.h"
#include "Constants.h"
#include "GameActivity.h"
#include "Scene.h"
#include "GameVersion.h"
#include "FrameMan.h"
#include "GUIInput.h"
#include "GnsTransport.h"
#include "LuaMan.h"
#include "NetHttpClient.h"
#ifdef CCCP_WITH_GNS
#include "GnsSignaling.h"
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
#endif
#include "ConsoleMan.h"
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
#include "NetRoundStartScripts.h"

#include "nlohmann/json.hpp"

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <list>
#include <optional>
#include <random>
#include <string>
#include <sstream>
#include <thread>
#include <utility>

std::string BuildLoopPaceJson();

namespace RTE {

	// A live host's answer to a held return its tail cannot reach: the seat comes back through the image.
	static constexpr const char* c_ImageRejoinDetail = "slow player: rejoin from the host's image";
	static constexpr const char* c_HistoryPassedDetail = "the world's history moved past this catch-up";

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
			text << "\nHolds " << peer.holds << " | AI substitutions " << peer.substitutions << " | Rejoins " << peer.rejoins
			     << " | Longest wait " << peer.longestWaitMs << " ms";
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

	namespace {
		/// A seat is away while its owner's link is gone, its return is in flight or the host opened it; a hold in place keeps its player.
		bool SeatViewAway(const NetMatchService::SeatView& view) {
			return view.peerId != 0 && (view.seat.owner == 0 || view.seat.link == NetSeatLink::Dropped || view.state == "Reconnecting");
		}

		/// The name a route receipt gives the relay offer a connection's TURN lists came from: its match id and its expiry.
		std::string RelayOfferName(const NetRelayConfig& offer) {
			return offer.matchId + "@" + std::to_string(offer.expiresAt);
		}
	} // namespace

	void NetMatchService::UpdateSummarySeatsLocked() {
		for (const auto& [peerId, view] : m_SeatViews) {
			auto& previous = m_SummarySeats[peerId];
			const bool away = SeatViewAway(view);
			const bool wasAway = SeatViewAway(previous);
			// Another seat on the slot (a world's promoted watcher, or a promotion undone) is counted under its own view.
			const bool otherSeat = previous.peerId != 0 && previous.stableSeat != view.stableSeat;
			if (!otherSeat && away && !wasAway) ++m_CurrentMatchSummary.drops;
			if (!otherSeat && previous.peerId != 0 && previous.seat.owner != 0 && view.seat.owner != 0 && view.seat.owner != previous.seat.owner) {
				++m_CurrentMatchSummary.substitutions;
			} else if (!otherSeat && wasAway && !away) {
				++m_CurrentMatchSummary.reclaims;
			}
			for (auto& peer : m_CurrentMatchSummary.peers) {
				if (peer.peerId != peerId) continue;
				peer.seat = view.stableSeat;
				if (!view.name.empty()) peer.name = view.name;
			}
			previous = view;
		}
	}

	NetMatchService::SeatView NetMatchService::BuildSeatView(uint8_t peerId, uint16_t stableSeat, uint32_t revision, const NetRosterSeat& seat, const std::string& name) {
		SeatView view;
		view.peerId = peerId;
		view.stableSeat = stableSeat;
		view.revision = revision;
		view.seat = seat;
		view.name = !seat.name.empty() ? seat.name : (name.empty() ? "Player " + std::to_string(peerId) : name);
		view.state = RosterSeatStateWord(seat);
		view.line = RosterSeatLine(seat, view.name);
		return view;
	}

	void NetMatchService::RefreshSeatViewsLocked(uint64_t observedAtMs) {
		const NetRosterReplica& replica = m_ReconnectClient.GetRosterReplica();
		const bool hosted = m_IsHost && m_AdmissionAttached;
		if (!hosted && !replica.HasRoster()) return;
		const NetSeatRoster& roster = hosted ? m_ReconnectHost.GetRoster() : replica.Roster();
		// Before its round runs a joiner's seats are the lobby's agreed config, which the publish has just adopted.
		const NetMatchConfig& config = m_State != NetMatchServiceState::Running && m_AdoptedMatchConfig.sessionId != 0 ? m_AdoptedMatchConfig
		                               : m_Runner ? m_Runner->GetMatchConfig() : (m_AdoptedMatchConfig.sessionId != 0 ? m_AdoptedMatchConfig : m_MatchConfig);
		std::map<uint8_t, SeatView> views = BuildSeatViews(roster, NetH4BuildSeatTable(config), config, m_LobbySnapshot.members);
		if (m_Coordinator) {
			std::map<uint8_t, std::string> names;
			for (const auto& [peerId, view]: views) names[peerId] = view.name;
			m_Coordinator->SetSeatNames(std::move(names));
		}
		std::map<uint8_t, SeatView> previous = std::move(m_SeatViews);
		m_SeatViews = std::move(views);
		RecordRosterTransitions(previous, observedAtMs);
	}

	std::map<uint8_t, NetMatchService::SeatView> NetMatchService::BuildSeatViews(const NetSeatRoster& roster, const std::vector<NetH4Seat>& table, const NetMatchConfig& config,
	                                                                             const std::vector<NetLobbyMember>& members) {
		std::map<uint8_t, SeatView> views;
		// A seat the roster binds to another slot plays that slot: its view is keyed there, and the slot's own seat steps aside.
		std::set<uint64_t> boundSlots;
		for (const NetRosterSeat& seat: roster.seats) if (seat.bindingRef != 0) boundSlots.insert(seat.bindingRef);
		for (const NetH4Seat& entry: table) {
			if (entry.cpu || entry.lockstepPeerId == 0) continue;
			const NetRosterSeat* seat = roster.Find(NetRosterIdOf(entry.stableSeat));
			if (!seat || (seat->bindingRef == 0 && boundSlots.contains(entry.lockstepPeerId))) continue;
			const uint8_t played = seat->bindingRef != 0 ? static_cast<uint8_t>(seat->bindingRef) : entry.lockstepPeerId;
			std::string name;
			const std::string open = NetMatchConfigUtil::UnseatedSlotName(played, config.persistentWorld);
			// A slot's configured name is the seat's own; a seat playing another slot is named by its player alone.
			if (seat->bindingRef == 0)
				for (const NetMatchPlayerSlot& slot: config.players)
					if (slot.peerId == played && !slot.displayName.empty() && !(config.persistentWorld && slot.displayName == open)) name = slot.displayName;
			for (const NetLobbyMember& member: members)
				if (member.peerId == played && !member.displayName.empty() && member.displayName != open)
					name = member.displayName;
			views[played] = BuildSeatView(played, entry.stableSeat, roster.revision, *seat, name);
		}
		return views;
	}

	std::map<uint8_t, NetMatchService::SeatView> NetMatchService::GetSeatViews() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_SeatViews;
	}

	std::optional<NetMatchService::SeatView> NetMatchService::GetSeatView(uint8_t peerId) const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		const auto found = m_SeatViews.find(peerId);
		if (found == m_SeatViews.end()) return std::nullopt;
		return found->second;
	}

	void NetMatchService::CaptureMatchSummaryLocked(const std::string& result) {
		// The first terminal result owns this round; Main's Complete paths quit after capturing it.
		if (m_State != NetMatchServiceState::Running || m_LastMatchSummary || m_CurrentMatchSummary.identityLine.empty()) return;
		UpdateSummarySeatsLocked();
		m_CurrentMatchSummary.result = result.empty() ? "Match complete" : result;
		const auto* activity = dynamic_cast<const GameActivity*>(g_ActivityMan.GetActivity());
		m_CurrentMatchSummary.winnerTeam = m_ReceivedEndWinner ? *m_ReceivedEndWinner : activity ? activity->GetWinnerTeam() : Activity::NoTeam;
		m_CurrentMatchSummary.runningTicks = m_ReceivedEndWinner ? m_CompletedRoundFinalFrame : ScenarioRunner::GetLockstepAppliedFrame();
		m_CurrentMatchSummary.paceJson = ::BuildLoopPaceJson();
		for (auto& peer: m_CurrentMatchSummary.peers) {
			NetLockstepPeerStats totals;
			if (const auto past = m_LockstepTotals.peers.find(peer.peerId); past != m_LockstepTotals.peers.end()) totals = past->second;
			if (m_Coordinator) {
				if (const auto live = m_Coordinator->GetStats().peers.find(peer.peerId); live != m_Coordinator->GetStats().peers.end()) {
					totals.holds += live->second.holds; totals.substitutions += live->second.substitutions; totals.rejoins += live->second.rejoins;
					totals.longestWaitMs = std::max(totals.longestWaitMs, live->second.longestWaitMs);
				}
				peer.inputDelayFrames = NetMatchConfigUtil::PeerInputDelay(m_Coordinator->GetConfig().matchConfig, peer.peerId);
			}
			peer.holds = totals.holds; peer.substitutions = totals.substitutions; peer.rejoins = totals.rejoins; peer.longestWaitMs = totals.longestWaitMs;
		}
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

	bool NetIcePrefersP2P(const NetIceJoinTarget& target, bool iceEnabled) {
		return iceEnabled && (target.joinMode == "ice" || target.joinMode == "either");
	}

	std::string NetIceMenuJoinAddress(const NetDirectoryClient::GameRow& row) {
		return row.source == "NET" && !row.sessionId.empty() ? "session:" + row.sessionId : row.address;
	}

	void NetReportGameData(const NetIdentityManifest& manifest) {
		for (const std::string& line : NetIdentity::DescribeGameData(manifest)) {
			g_ConsoleMan.PrintString(line);
			System::PrintDiagnosticLine("[net-identity] " + line.substr(line.find_first_not_of(' ')));
		}
	}

	std::string NetIceConnectingLine(uint64_t elapsedMs, uint64_t limitMs, bool hostAnswered, bool relayReady, bool retrying) {
		// The clock leads, so a status line elided to its box still says how long the wait may last.
		const std::string opening = std::string(retrying ? "Connecting again" : "Connecting") + " (" + std::to_string(elapsedMs / 1000) + " of " + std::to_string(limitMs / 1000) + " s) - ";
		if (!hostAnswered) return opening + "waiting for the host's answer";
		return opening + "testing routes" + (relayReady ? ", relay ready" : "");
	}

	bool NetIceRetryCanSucceed(uint64_t signalsFromHost, uint64_t refusals) {
		return signalsFromHost > 0 && refusals == 0;
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

	std::string NetIceResolveSessionRow(const std::vector<NetDirectorySessionRow>& rows, const NetDirectoryLocalIdentity& local, const std::string& sessionId, NetIceJoinTarget* out, const NetDirectoryLocalIdentity* worldLocal, bool reservedSeat) {
		for (const NetDirectorySessionRow& row : rows) {
			if (row.sessionId != sessionId) {
				continue;
			}
			// The join list decides joinability, so a session-id join is refused with its labels.
			const std::vector<NetDirectoryClient::GameRow> merged = NetDirectoryClient::MergeGameLists({}, {row}, local, worldLocal);
			if (merged.empty()) {
				break;
			}
			if (!merged.front().joinable && !(reservedSeat && merged.front().reason == "full")) {
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
	std::string NetMatchService::s_HostBanStorePath;
	std::string NetMatchService::s_JoinWaitPath;
	bool NetMatchService::s_ApplyForSeat = false;
	uint16_t NetMatchService::s_ApplySeat = 0;
	bool NetMatchService::s_ApplyOnce = false;
	uint16_t NetMatchService::s_ApplyOnceSeat = c_NetH4AnySubstitutableSeat;
	bool NetMatchService::s_WaitForSlotOnce = false;
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
		bool ResumeBytes(const std::string& hex, std::vector<uint8_t>& out);

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
				{"answered_transactions_dropped", stats.answeredTransactionsDropped},
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

	void NetMatchService::RequestHostPortMap(uint16_t port, NetPortMapWan* wan) {
		NetPortMap::Options options = NetPortMap::ProbeOverrides();
		if (wan) options.wan = wan;
		s_PortMap.Request(port, NetPortMap::c_DefaultLeaseS, options);
		s_PortMapRequested = true;
		s_PortMapApplied = false;
	}

	void NetMatchService::ReleaseHostPortMap() {
		s_PortMapRequested = false;
		s_PortMap.Release();
		s_PortMapApplied = false;
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
		// A seat rejoining the match it was playing is still that match's: a failed attempt is retried, never dropped.
		bool rejoinOfARunningMatch = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			rejoinOfARunningMatch = request.rejoin && !request.host && m_MatchWasRunning;
		}
		Destroy();
		if (rejoinOfARunningMatch) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_MatchWasRunning = true;
			m_RejoinOfRunningMatch = true;
		}
		m_CancelRequested.store(false);
		// A seat rejoining its own running match has nothing to choose in the lobby round: it is ready once it connects.
		m_ReadyRequested.store(rejoinOfARunningMatch);
		m_StartRequested.store(false);
		if (request.dedicated && !request.host) {
			if (error) *error = "dedicated service requires the host role";
			return false;
		}
		if (request.port == 0) {
			if (error) *error = "port must be nonzero";
			return false;
		}
		// Both roles, before anything is opened: a runtime without threaded Lua states cannot match one.
		if (const std::string luaRefusal = NetIdentity::LocalMatchRefusal(static_cast<int>(g_LuaMan.GetThreadedScriptStates().size())); !luaRefusal.empty()) {
			if (error) *error = luaRefusal;
			SetState(NetMatchServiceState::Failed, request.host ? "Hosting refused" : "Joining refused", luaRefusal);
			return false;
		}
		if (!request.host && request.address.empty() && request.sessionId.empty()) {
			if (error) *error = "join address must not be empty";
			return false;
		}
		if (!request.host && request.sessionId.empty() && g_SettingsMan.GetNetworkConnectionMode() == SettingsMan::NetworkConnectionMode::RelayOnly) {
			if (error) *error = "Relay only requires an Internet session; select a game from the Internet list";
			return false;
		}
		if (g_SettingsMan.GetNetworkConnectionMode() == SettingsMan::NetworkConnectionMode::RelayOnly &&
		    (g_SettingsMan.GetSessionDirectoryUrl().empty() || (request.host && !g_SettingsMan.GetNetworkIceEnable()))) {
			if (error) *error = "Relay only needs a session directory and NAT traversal enabled";
			return false;
		}
		if (request.host && g_SettingsMan.GetNetworkHostRelayMode() == SettingsMan::NetworkHostRelayMode::Fixed &&
		    NetRelayConfig::Fixed(g_SettingsMan.GetNetworkTurnServers(), g_SettingsMan.GetNetworkTurnUser(), g_SettingsMan.GetNetworkTurnPass(), "host", UINT64_MAX).Empty()) {
			if (error) *error = "Fixed relay needs a valid address, username and password in Host Options > Network";
			return false;
		}
		// Past the refusals: the settings are read once here, where a real host starts, and ride the
		// request to both roster builds, so the worker's copy cannot pick up a later menu edit.
		SeatSavedOptions(request);
		if (!request.host) m_LastJoinRoute = request;
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
		// A world boot resumes its own checkpoint chain by default, through the very same resume path.
		if (request.host && !ResolveWorldResume(request, error)) {
			SetState(NetMatchServiceState::Failed, "World resume refused", error ? *error : "world resume refused");
			return false;
		}
		// A resume reopens the lobby the checkpoint was written under, so its own manifest authors the
		// roster and the request's activity, scene and seats are taken from it.
		if (request.host && !request.resumeMatchId.empty() && !PrepareResume(request, error)) {
			SetState(NetMatchServiceState::Failed, "Resume refused", error ? *error : "resume refused");
			return false;
		}
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
			if (request.resumeConfig) {
				SeatResumedWorldConfig(*request.resumeConfig, m_WorldIdentity);
			}
			// A world keeps the capacity its first boot authored; a later boot only names one when the record has none.
			if (!request.worldTeamCapacity.has_value() && WorldIdentityCarriesCapacity(m_WorldIdentity)) {
				request.worldTeamCapacity = m_WorldIdentity.teamCapacity;
				request.worldMaxSpectators = m_WorldIdentity.maxSpectators;
				request.worldRespawnDelaySeconds = m_WorldIdentity.respawnDelaySeconds;
			}
			m_WorldJoin.SetIdentityPath(NetWorldIdentityFile::DefaultPath());
			// The writer thread hashes what it wrote; a multi-megabyte digest is not sim-thread work.
			g_ActivityMan.SetAutosaveDigest([](const std::vector<uint8_t>& bytes) { return DigestWorldJoinBytes(bytes); });
			{
				std::ostringstream line;
				line << "[net-world] identity " << m_WorldIdentity.worldId << " boot=" << m_WorldIdentity.boot
			          << " round=" << m_WorldIdentity.round;
				System::PrintDiagnosticLine(line.str());
			}
		}
		if (request.resumeConfig) {
			matchConfig = *request.resumeConfig;
		} else if (!BuildMatchConfig(request, c_UiSessionId, matchConfig, &configError)) {
			if (error) *error = configError;
			SetState(NetMatchServiceState::Failed, "Match roster refused", configError);
			return false;
		}
		if (matchConfig.persistentWorld && !WorldIdentityCarriesCapacity(m_WorldIdentity)) {
			// The first boot's capacity becomes the world's own, so every later boot offers the same seats.
			m_WorldIdentity.teamCapacity = matchConfig.worldTeamCapacity;
			m_WorldIdentity.maxSpectators = matchConfig.worldMaxSpectators;
			m_WorldIdentity.respawnDelaySeconds = matchConfig.worldRespawnDelaySeconds;
			if (std::string writeError; !NetWorldIdentityFile::Write(NetWorldIdentityFile::DefaultPath(), m_WorldIdentity, &writeError)) {
				{
					std::ostringstream line;
					line << "[net-world] identity capacity not stored: " << writeError;
					System::PrintDiagnosticLine(line.str());
				}
			}
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
		if (!NetIdentity::CaptureManifestInputs(manifest, &buildError, identityOptions)) {
			if (error) *error = buildError;
			SetState(NetMatchServiceState::Failed, "Identity build failed", buildError);
			return false;
		}

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
		const bool icePreference = request.host || g_SettingsMan.HasNetworkIceEnableOverride() ? g_SettingsMan.GetNetworkIceEnable() : true;
		const bool iceEnabled = icePreference && directoryConfigured &&
		                        (request.host || !request.sessionId.empty());
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_IceEnabled = iceEnabled;
			m_IceBoundSessionId.clear();
			m_IceIdentity.clear();
			m_IceJoinSessionId = request.sessionId;
			m_IceReport.clear();
			m_IceRoute.clear();
			SetRelayOfferLocked({});
			m_RelayError.clear();
			m_RelayReady = false;
			m_RelayPublishPending = false;
			m_RelayAttempted = false;
			m_RelayReplies = m_Directory.IceReplies();
			m_NextRelayRequestMs = 0;
			m_FreshRelayRequested = true;
			m_HostRelayMode = static_cast<int>(g_SettingsMan.GetNetworkHostRelayMode());
			m_ConnectionMode = static_cast<int>(g_SettingsMan.GetNetworkConnectionMode());
			m_FixedRelayOffer = NetRelayConfig::Fixed(g_SettingsMan.GetNetworkTurnServers(), g_SettingsMan.GetNetworkTurnUser(), g_SettingsMan.GetNetworkTurnPass(),
			                                         "host", UnixNowMs(nullptr) / 1000 + NetDirectoryClient::c_MaxRelayTtlSeconds);
			if (m_HostRelayMode == 2) {
				SetRelayOfferLocked(m_FixedRelayOffer);
				if (m_RelayOffer.Empty()) m_RelayError = "Fixed relay needs an address, username and password";
			}
			m_DirectorySessionId.clear();
			m_DirectoryToken.clear();
			m_DirectoryRegistered = false;
			m_MigrationKey = {};
			m_MigrationAdmissionState.clear();
			m_LastMigrationAdmissionState.clear();
			m_MigrationAuthority = 0;
			m_MigrationMembers.clear();
			m_MigrationDirectorySession.clear();
			m_MigrationDirectoryToken.clear();
			m_MigrationDirectoryResumePending = false;
			m_MigrationStatusUntilMs = 0;
			m_HeldHostStatusAtMs = 0;
			m_MigrationFallbackReady = false;
			m_MigrationFallbackCoordinator.reset();
			m_MigrationGeneration = 0;
			m_MigrationRepairPending = false;
			m_AutosaveMatchId.clear();
			m_NextAutosaveSimTime = -1;
			m_LastAutosaveSimTime = -1;
			ResetCheckpointSchedule();
			m_FinalCheckpointWritten = false;
			m_ResumeSegmentTick = 0;
			m_WorkerDone = false;
			m_IdentityPending = true;
			m_IsHost = request.host;
			m_CurrentMatchSummary = {};
			m_SummarySeats.clear();
			// A resumed match keeps the seat the agreed configuration gave its host, which a handover
			// may have moved off peer 1 before the match died.
			m_LocalPeerId = request.host ? (request.resumeConfig ? matchConfig.hostPeerId : 1) : 2;
			m_LocalTeam = request.dedicated ? Activity::NoTeam : (request.host ? 0 : 1);
			m_Dedicated = request.dedicated;
			m_HumanSeats = static_cast<int>(std::count_if(matchConfig.players.begin(), matchConfig.players.end(), [](const auto& slot) { return !slot.cpu; }));
			m_MatchConfig = matchConfig;
			m_InputDelayText.clear();
			m_ResyncOnDesync = request.resyncOnDesync;
			m_PendingResyncLoad.clear();
			m_BeaconGamePort = request.port;
			m_BeaconMaxPlayers = static_cast<uint8_t>(std::max(1, m_HumanSeats - (request.dedicated ? 1 : 0)));
			m_LocalName = request.playerName.empty() ? (request.host ? "Host" : "Client") : request.playerName;
			m_JoinRefusalKey.clear();
			m_JoinRefusalSeat.reset();
			// A new join is not the substitute this process was; its own rejoin carries on as one.
			if (!request.rejoin) m_SubstituteRejoinStarted = false;
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
					// Nothing is connected yet, so every watcher slot the world offers is free.
					m_DirectoryRow.spectatorFree = static_cast<int64_t>(WorldSpectatorBound(matchConfig));
					m_DirectoryRow.spectatorMax = static_cast<int64_t>(WorldSpectatorBound(matchConfig));
				}
			}
			if (request.host && !request.resumeMatchId.empty()) {
				// The resumed match keeps writing its checkpoints under the id it already has, so one
				// chain of checkpoints survives however often the host restarts.
				m_AutosaveMatchId = m_ResumeMatchId;
				m_AutosaveIdentity.sessionId = matchConfig.sessionId;
				m_AutosaveIdentity.roundId = m_ResumeRoundId;
				m_AutosaveIdentity.intervalSeconds = m_ResumeIntervalSeconds;
				m_AutosaveIdentity.pinnedCheckpointSource = m_PinnedAutosave;
				m_PinnedAutosave->Store(m_ResumeRoundId, m_ResumeTick);
				m_MatchAutosaveSeconds = MatchAutosaveSeconds(matchConfig);
				// This world's round opens ON that checkpoint, so its recording is a segment standing on
				// it from the first frame instead of a file that names no world.
				if (matchConfig.persistentWorld) m_ResumeSegmentTick = m_ResumeTick;
				if (!matchConfig.persistentWorld) {
					// The stored row token resumes the very session id the peers' tickets name.
					m_DirectoryRow.resumeSessionId = m_ResumeDirectorySession;
					m_DirectoryRow.resumeToken = m_ResumeDirectoryToken;
				}
				// The row advertises the seats the resumed state really leaves open, not peerCount-1:
				// a returning player must not read a world of free seats that are all still held.
				const int64_t openSeats = NetReconnectHost::CountExportedOpenSeats(m_ResumeAdmissionState, matchConfig.hostPeerId);
				if (openSeats >= 0) {
					m_DirectoryRow.seatsFree = openSeats;
				}
			}
			m_DirectoryRetracted = false;
			m_DirectoryHidden = false;
			m_KeepEndedDirectoryLease = false;
			m_DirectoryRelistPending = false;
		}
		if (request.host && g_SettingsMan.GetNetworkPortMapEnable()) {
			RequestHostPortMap(request.port, nullptr);
		} else {
			ReleaseHostPortMap();
		}
		{
			// A fresh lockstep round starts every peer on the host's script state, never on each machine's own history.
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_RoundStartScriptsWanted = request.host && !request.resumeConfig && !targetingWorld;
			m_RoundStartScripts.clear();
			m_RoundStartScriptsToStream.clear();
			m_PendingRoundStartScripts.clear();
		}
		m_EverStarted.store(true);
		m_Worker = std::thread(&NetMatchService::WorkerMain, this, request, std::move(manifest), std::move(identityOptions));
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
		bool departedHost = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			// A persistent world never rematches: its end is the world closing, and every seat lands on it as on its host leaving.
			const NetMatchConfig& round = m_Runner ? m_Runner->GetMatchConfig() : m_AdoptedMatchConfig.sessionId != 0 ? m_AdoptedMatchConfig : m_MatchConfig;
			if (m_State == NetMatchServiceState::Completed && round.persistentWorld) {
				m_State = NetMatchServiceState::Failed;
				m_ErrorText = m_IsHost ? "The world is closed" : "The host left the match";
				m_StatusText = m_ErrorText;
				if (!m_IsHost) NoteHostEndedTheMatchLocked();
				if (error) *error = m_ErrorText;
				return false;
			}
			if (m_State != NetMatchServiceState::Completed || !ActiveWireLocked() || !m_Session || !m_Runner) {
				if (error) *error = "no completed match to rematch";
				return false;
			}
			if (m_LeftMatch || m_PendingLobbyOverflow) {
				if (error) *error = m_LeftMatch ? "this match was left" : "the rematch lobby queue overflowed";
				return false;
			}
			if (HostOptionsNeedCorrectionLocked()) {
				if (error) *error = m_ErrorText;
				return false;
			}
			DrainPendingSessionEventsLocked(false);
			departedHost = !m_IsHost || (m_MigrationGeneration != 0 && m_MigrationMembers.size() == 1);
			if (!m_Session->IsReady()) {
				// Session lost (the other player quit); settle so the UI stops offering a rematch.
				m_State = NetMatchServiceState::Failed;
				// A link the host closed to bring this seat back through its image keeps the seat: its ticket returns it.
				m_RematchReturnOwed = !m_IsHost && !m_HostEndedTheMatch && m_Session->HasReject() && m_Session->BuildRejectText().find(c_ImageRejoinDetail) != std::string::npos;
				{
					std::ostringstream line;
					line << "[net-match] rematch unavailable: " << m_Session->BuildRejectText() << (m_RematchReturnOwed ? "; the seat returns through its ticket" : "");
					System::PrintDiagnosticLine(line.str());
				}
				if (m_RematchReturnOwed) {
					m_ErrorText = "Could not reach the host - retrying";
					m_StatusText = m_ErrorText;
					if (error) *error = m_ErrorText;
					return false;
				}
				m_ErrorText = departedHost ? "The host left the match" : "The other players left the match";
				m_StatusText = m_ErrorText;
				// The host ended a match this seat had finished: the landing says so, and nothing reconnects to it.
				if (departedHost && !m_IsHost) NoteHostEndedTheMatchLocked();
				if (error) *error = m_ErrorText;
				return false;
			}
			m_FreshRelayRequested = true;
			m_EndRecordSent.clear();
			m_ToldMatchOver.clear();
			m_EndWinnerTeam = Activity::NoTeam;
			m_ReceivedEndWinner.reset();
			m_PendingResyncState.reset();
			m_ResyncRetainsLocalState = false;
			m_ResyncSourceRound = 0;
			m_HostRepairPending = false;
			m_HostRepairDeferred = false;
			m_MigrationAuthority = 0;
			m_MigrationMembers.clear();
			m_MigrationGeneration = 0;
			m_MigrationRepairPending = false;
			EndRoundCatchUpLocked();
			// The rejoin plane and its committed tail belong to the round that ended: the next round opens its own.
			if (m_WorldJoin.IsPrivateMatch()) {
				m_WorldJoin.Reset();
				m_PrivateActivations.clear(); m_PrivateJoinBlobs.clear(); m_InPlaceIncarnationBumps.clear();
				m_PrivateBaseRequested = false; m_PrivateBaseTick = 0; m_PrivateBasePending.reset(); m_PrivateJoinError.clear();
			}
			if (m_IsHost)
				(void)GetNetAuthCrypto().RandomBytes(m_MigrationKey.data(), m_MigrationKey.size());
			// The next round keeps every seat of this one. A client that left its round held holds no view of it - its rejoin
			// replaced the round's config - so it takes the host's roster as offered.
			m_Runner->SetRematchOwed(m_IsHost || !m_LeftRoundHeld);
			m_LeftRoundHeld = false;
			DrainPendingSessionEventsLocked(false);
			AccumulateLockstepTotalsLocked();
			link = TakeTransportLinkLocked();
			session = std::move(m_Session);
			m_WorkerSession = session.get();
			NoteSessionHandedToWorker(*session);
			runner = std::move(m_Runner);
			if (m_IsHost) {
				// A rematch restarts from a zeroed sim count.
				runner->SetStartFrame(1);
			}
			// A rematch round starts every peer on the host's script state as the first round did, so the
			// host takes it again at the next start and each peer's own history of the last round is dropped.
			m_RoundStartScripts.clear();
			m_RoundStartScriptsToStream.clear();
			m_PendingRoundStartScripts.clear();
			m_Coordinator.reset();
			m_RoundStartScriptsWanted = m_IsHost;
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
		m_Worker = std::thread(&NetMatchService::WorkerRematchMain, this, std::move(link), session.release(), coordinator.release(), runner.release(), departedHost);
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
			if (m_IsHost && m_Coordinator) {
				const uint64_t resume = m_Coordinator->GetResumeFrame();
				AnswerEndedReturnersLocked(m_CompletedRoundFinalFrame != 0 ? m_CompletedRoundFinalFrame : (resume > 0 ? resume - 1 : 0));
			}
		}
		// The main loop owns coordinator teardown.
		Complete(text);
	}

	std::string NetMatchService::MatchOverGoodbyeText(uint64_t finalFrame) {
		return finalFrame == 0 ? std::string("match over") : "match over through frame " + std::to_string(finalFrame);
	}

	void NetMatchService::SayGoodbyeToRejoinersLocked() {
		if (!m_IsHost || !m_Coordinator) return;
		const uint64_t resume = m_Coordinator->GetResumeFrame();
		m_CompletedRoundFinalFrame = resume > 0 ? resume - 1 : 0;
		m_HostGoodbyeSeen = true;
		// Only a held seat can still be handshaking its way back; a member reads the round's own Stop and a
		// lobby peer is owed the next round.
		if (!m_Coordinator->AnyHeldAISeat()) return;
		m_GoodbyeOwedToRejoiners = true;
		// That seat is not a member, so the Stop never reaches it. Told the round is over and how it ended, it shows the
		// result and stays for the rematch; one still in its handshake is answered once it is in.
		AnswerEndedReturnersLocked(m_CompletedRoundFinalFrame);
	}

	bool NetMatchService::NoteHostGoodbyeLocked(const NetSession* session) {
		if (!session || !session->HasReject()) return m_HostGoodbyeSeen;
		// The host's own words are the goodbye. A transport that carries the refusal as a plain disconnect
		// reports its own reason code, so the text is what identifies a finished round.
		const std::string& summary = session->GetRejectSummary();
		if (summary.rfind("match over", 0) != 0) return m_HostGoodbyeSeen;
		m_HostGoodbyeSeen = true;
		const size_t space = summary.find_last_of(' ');
		uint64_t parsed = 0;
		bool digits = false;
		for (size_t i = space == std::string::npos ? summary.size() : space + 1; i < summary.size(); ++i) {
			if (summary[i] < '0' || summary[i] > '9') { digits = false; break; }
			parsed = parsed * 10 + static_cast<uint64_t>(summary[i] - '0');
			digits = true;
		}
		if (digits) m_CompletedRoundFinalFrame = parsed;
		return true;
	}

	bool NetMatchService::MatchPlaysOnUnderANewHost(const NetSeatRoster& roster) {
		return std::any_of(roster.seats.begin(), roster.seats.end(), [&roster](const NetRosterSeat& seat) {
			return seat.seatId != roster.hostSeat && seat.owner != 0 && seat.link == NetSeatLink::Connected;
		});
	}

	std::string NetMatchService::RoundEndResultText(int winnerTeam, int localTeam) {
		if (winnerTeam < 0) return "Match over: draw";
		if (localTeam == Activity::NoTeam) return "Match over";
		return winnerTeam == localTeam ? "Victory!" : "Defeat";
	}

	bool NetMatchService::TakeRoundEndRecord(uint64_t& record) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_RoundEndRecord) return false;
		record = *std::exchange(m_RoundEndRecord, std::nullopt);
		m_LeftRoundHeld = true;
		return true;
	}

	bool NetMatchService::HostGoodbyeSeen(uint64_t& finalFrame) const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		finalFrame = m_CompletedRoundFinalFrame;
		return m_HostGoodbyeSeen;
	}

	uint64_t NetMatchService::RejoinProgressSum() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		uint64_t sum = 0;
		// A rejoin begins as a connection in its handshake and becomes a seat the coordinator does not yet
		// use: both are the admission arriving, before any world-join session exists to measure.
		if (m_Session) {
			sum += m_Session->GetHandshakingPeerCount();
			for (const NetSessionPeerInfo& peer: m_Session->GetReadyPeers())
				if (!m_Coordinator || !m_Coordinator->UsesTransportPeer(peer.transportPeerId)) sum += 1;
		}
		for (const auto& session: m_WorldJoin.Sessions()) {
			// Every step a rejoin takes counts: it was admitted, it moved a phase, an image was staged for it,
			// it acknowledged another chunk, it consumed another tail frame.
			sum += 1 + static_cast<uint64_t>(session.phase) + session.incarnation + session.snapshotTick +
			       (session.transferStarted ? 1 : 0) + session.ackedChunks + session.deliveredThrough +
			       session.acknowledgedThrough + session.catchUpTicks + session.acknowledgedActivation +
			       (session.activationCommitted ? 1 : 0);
		}
		return sum;
	}

	bool NetMatchService::SeatMidAdmission(uint64_t frame) const {
		NetLockstepPlane::Gap plane("admission wait");
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_IsHost || !m_Coordinator || m_State == NetMatchServiceState::Completed || m_HostGoodbyeSeen ||
		    (g_ActivityMan.GetActivity() && g_ActivityMan.GetActivity()->IsOver())) return false;
		// A seat the AI holds for its player is coming back until it is taken back or released, its link up or not.
		if (m_Coordinator->AnyHeldAISeat() || (m_Session && m_Session->GetHandshakingPeerCount() > 0)) return true;
		return std::any_of(m_WorldJoin.Sessions().begin(), m_WorldJoin.Sessions().end(), [frame](const auto& session) {
			if (session.phase == NetWorldJoinPhase::Failed || session.phase == NetWorldJoinPhase::Spectating) return false;
			return session.phase != NetWorldJoinPhase::Active || session.activationTick > frame;
		});
	}

	void NetMatchService::SetRejoinPhaseLocked(NetSession::RejoinPhase phase) {
		const NetSession* live = m_Session ? m_Session.get() : m_WorkerSession;
		if (live && live->GetRejoinPhase() != phase) {
			System::PrintDiagnosticLine(std::string("[net-match] rejoin phase ") + NetSession::RejoinPhaseName(live->GetRejoinPhase()) + " -> " + NetSession::RejoinPhaseName(phase));
		}
		if (m_Session) m_Session->SetRejoinPhase(phase);
		if (m_WorkerSession && m_WorkerSession != m_Session.get()) m_WorkerSession->SetRejoinPhase(phase);
	}

	bool NetMatchService::EndedRoundOwesGoodbye(bool coordinatorUsesPeer, bool seatUnderAIAtEnd) {
		// A seat readmitted for a frame the round never reached ended it under the AI: it never played again either.
		return !coordinatorUsesPeer || seatUnderAIAtEnd;
	}

	bool NetMatchService::EndedRoundAwaitsFinalTail(bool joining, bool seatUnderAIAtEnd, bool privateMatch, uint64_t tailLastFrame, bool toldMatchOver) {
		return !joining && !toldMatchOver && seatUnderAIAtEnd && privateMatch && tailLastFrame != 0;
	}

	void NetMatchService::RefuseEndedPeersLocked(const std::string& reason) {
		if (!m_IsHost || !m_Session || !m_Coordinator) return;
		RefuseEndedPeers(*m_Session, *m_Coordinator, reason, false);
	}

	void NetMatchService::RefuseEndedPeers(NetSession& session, const NetLockstepCoordinator& coordinator, const std::string& reason, bool joiningToo) {
		const uint64_t resume = coordinator.GetResumeFrame();
		const uint64_t lastFrame = resume > 0 ? resume - 1 : 0;
		for (const NetSessionPeerInfo& peer : session.GetReadyPeers()) {
			uint8_t seat = 0;
			for (const auto& [peerId, transport]: coordinator.RemoteTransports()) if (transport == peer.transportPeerId) seat = peerId;
			if (EndedRoundOwesGoodbye(coordinator.UsesTransportPeer(peer.transportPeerId), seat != 0 && coordinator.IsSeatUnderAI(seat, lastFrame))) {
				session.DisconnectReadyPeer(peer.transportPeerId, NetRejectReason::SessionEnded, reason);
			}
		}
		// A returning seat still in its handshake is owed the same answer, or it reads the host leaving as a lost link.
		if (joiningToo) session.DisconnectJoiningPeers(NetRejectReason::SessionEnded, reason);
	}

	void NetMatchService::AnswerEndedReturnersLocked(uint64_t finalFrame) {
		if (!m_IsHost || !m_Session || !m_Coordinator) return;
		PumpWorldJoinLobby(AdmissionNowMs());
		if (const auto* activity = dynamic_cast<const GameActivity*>(g_ActivityMan.GetActivity()); activity && activity->IsOver()) m_EndWinnerTeam = activity->GetWinnerTeam();
		const uint64_t resume = m_Coordinator->GetResumeFrame();
		const uint64_t lastFrame = resume > 0 ? resume - 1 : 0;
		for (const NetSessionPeerInfo& peer : m_Session->GetReadyPeers()) {
			if (m_EndRecordSent.contains(peer.transportPeerId)) continue;
			uint8_t seat = 0;
			for (const auto& [peerId, transport]: m_Coordinator->RemoteTransports()) if (transport == peer.transportPeerId) seat = peerId;
			if (seat == 0) {
				const std::optional<uint16_t> stable = m_ReconnectHost.StableSeatOfConnection(peer.transportPeerId);
				for (const auto& held: m_ReconnectHost.GetSeatStatuses()) if (stable && held.stableSeat == *stable) seat = held.lockstepPeerId;
			}
			if (!EndedRoundOwesGoodbye(m_Coordinator->UsesTransportPeer(peer.transportPeerId), seat != 0 && m_Coordinator->IsSeatUnderAI(seat, lastFrame))) continue;
			if (seat != 0 && EndedRoundAwaitsFinalTail(m_WorldJoin.FindSession(peer.transportPeerId) != nullptr, m_Coordinator->IsSeatUnderAI(seat, lastFrame),
			                                           m_WorldJoin.IsPrivateMatch(), m_WorldJoin.Tail().LastFrame(), m_ToldMatchOver.contains(peer.transportPeerId))) {
				const std::string reason = "the ended round awaits its held seat's final-tail request";
				if (m_PrivateTransferHeldReasons[peer.transportPeerId] != reason) {
					m_PrivateTransferHeldReasons[peer.transportPeerId] = reason;
					System::PrintDiagnosticLine("[net-match] end waits peer=" + std::to_string(seat) + " final=" + std::to_string(finalFrame) + " tail=" + std::to_string(m_WorldJoin.Tail().LastFrame()));
				}
				continue;
			}
			// The final tail reaches a held seat before the record that closes its lobby.
			if (const auto* returning = m_WorldJoin.FindSession(peer.transportPeerId); returning && m_Runner) {
				if (returning->phase == NetWorldJoinPhase::SnapshotTransfer) continue;
				if (returning->phase == NetWorldJoinPhase::CatchingUp && returning->acknowledgedThrough < finalFrame) {
					if (!m_WorldJoin.BeginFinalTail(peer.transportPeerId, finalFrame)) continue;
					for (int part = 0; part < 32 && returning->deliveredThrough < finalFrame; ++part) {
						std::vector<uint8_t> chunk;
						if (!m_WorldJoin.NextTailChunk(peer.transportPeerId, chunk) ||
						    !m_Runner->GetLobbySession().SendPayloadTo(WorldJoinLobbyPeer(*returning), MakeWorldTailChunk(m_WorldJoin.TailRound(), chunk), nullptr)) break;
						m_WorldJoin.NoteTailChunkSent(peer.transportPeerId, chunk.size());
					}
					if (returning->deliveredThrough < finalFrame) continue;
				}
			}
			m_WorldJoin.CancelJoin(peer.transportPeerId, "the round ended");
			const bool answered = m_Runner && m_Runner->GetLobbySession().BindLateRemote(c_WorldRefusalLobbyPeer, peer.transportPeerId, nullptr) &&
			                      m_Runner->GetLobbySession().SendPayloadTo(c_WorldRefusalLobbyPeer, MakeWorldJoinReport(c_NetWorldReportRoundEnded, PackRoundEndedRecord(finalFrame, m_EndWinnerTeam)), nullptr);
			if (!answered) {
				m_Session->DisconnectReadyPeer(peer.transportPeerId, NetRejectReason::SessionEnded, MatchOverGoodbyeText(finalFrame));
				continue;
			}
			m_EndRecordSent.insert(peer.transportPeerId);
			System::PrintDiagnosticLine("[net-match] end record sent peer=" + std::to_string(peer.assignedPeerId + 1) + " connection=" + std::to_string(peer.transportPeerId) +
			                            " final=" + std::to_string(finalFrame) + " winner_team=" + std::to_string(m_EndWinnerTeam));
		}
		if (!m_EndRecordSent.empty()) m_GoodbyeOwedToRejoiners = std::any_of(m_WorldJoin.Sessions().begin(), m_WorldJoin.Sessions().end(), [](const auto& session) {
			return session.phase == NetWorldJoinPhase::SnapshotTransfer || session.phase == NetWorldJoinPhase::CatchingUp;
		}) || m_Session->GetHandshakingPeerCount() > 0;
	}

	uint64_t NetLobbyLastStateTransferMs();

	// The close the transport records when this peer stops it itself.
	static constexpr const char* c_OwnTransportStopDetail = "transport stopped";

	// A client's session has exactly one remote - the host. Its loss is the host's departure unless the
	// host's own record says it removed or refused this seat, which keeps its own text.
	static bool ClientSessionLossIsHostDeparture(const NetSession& session) {
		if (session.IsReady()) return false;
		if (!session.HasReject()) return true;
		switch (session.GetRejectReason()) {
			case NetRejectReason::SessionEnded:
			case NetRejectReason::Timeout:
			case NetRejectReason::InternalError:
			case NetRejectReason::HostLinkLost:
				return true;
			default:
				return false;
		}
	}

	bool NetMatchService::CanResyncLocked(std::string* error) {
		if (m_State != NetMatchServiceState::Running || !ActiveWireLocked() || !m_Session || !m_Runner) {
			if (error) *error = "no live match to resync";
			return false;
		}
		if (!m_Session->IsReady()) {
			m_State = NetMatchServiceState::Failed;
			if (!m_IsHost && ClientSessionLossIsHostDeparture(*m_Session)) {
				m_StatusText = "The host left the match";
				m_ErrorText = m_StatusText;
			} else {
				m_StatusText = "Resync unavailable";
				m_ErrorText = m_Session->HasReject() ? m_Session->BuildRejectText() : "the session was lost";
			}
			if (error) *error = m_ErrorText;
			return false;
		}
		return true;
	}

	bool NetMatchService::CanResyncMatch() const {
		// The frame's menus call this inside the plane's window, and it reads the round without the plane's lock.
		NetLockstepPlane::Gap plane("repair availability");
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_State == NetMatchServiceState::Running && ActiveWireLocked() && m_Session && m_Runner &&
		       m_Session->IsReady() && m_Coordinator && m_Coordinator->IsRunning() && !m_Coordinator->HasPendingRecoveryStop() &&
		       !m_ResyncHealOpen && m_PendingResyncLoad.empty() && !m_PendingAutosaveLoad;
	}

	bool NetMatchService::RequestHostRepair(std::string* error) {
		// The frame's menus call this inside the plane's window, and it reads the round without the plane's lock.
		NetLockstepPlane::Gap plane("host repair");
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto refuse = [error](const char* reason) {
			if (error) *error = reason;
			return false;
		};
		if (!m_IsHost) return refuse("only the host repairs the match");
		if (!CanResyncLocked(error)) return false;
		if (!m_Coordinator || !m_Coordinator->IsRunning() || m_Coordinator->HasPendingRecoveryStop() ||
		    m_ResyncHealOpen || !m_PendingResyncLoad.empty() || m_PendingAutosaveLoad) {
			return refuse("a recovery is already pending");
		}
		if (!m_ResyncOnDesync) return refuse("this session cannot reload a live snapshot");
		if (!ResyncSnapshotAllowed(g_ActivityMan.GetActivity())) return refuse("match over");
		// A repair restarts the round under every seat; a returner still catching up is let back in first.
		if (PrivateReturnerInFlightLocked()) {
			m_HostRepairDeferred = true;
			m_HostRepairDeferredMs = SteadyNowMs();
			m_HostRepairPending = true;
			System::PrintDiagnosticLine("[net-match] repair waits for a returning seat's catch-up");
			return true;
		}
		m_Coordinator->RequestResync("host requested repair");
		m_ResyncHealStartMs = SteadyNowMs();
		m_HostRepairPending = true;
		return true;
	}

	void NetMatchService::GetResyncStatus(bool* inFlight, uint64_t* bytes, uint64_t* elapsedMs) const {
		// The frame's menus call this inside the plane's window, and it reads the round without the plane's lock.
		NetLockstepPlane::Gap plane("repair status");
		std::lock_guard<std::mutex> lock(m_Mutex);
		const bool queued = m_HostRepairPending && m_State == NetMatchServiceState::Running && m_Coordinator && !m_HostRepairDeferred &&
		                    m_Coordinator->IsRunning() && m_Coordinator->HasPendingRecoveryStop();
		if (inFlight) *inFlight = m_ResyncHealOpen || queued || m_HostRepairDeferred;
		if (bytes) *bytes = queued ? 0 : (m_LastResync.envelopeBytes ? m_LastResync.envelopeBytes : m_LastResync.archiveBytes);
		if (elapsedMs) *elapsedMs = m_ResyncHealOpen || queued ? SteadyNowMs() - m_ResyncHealStartMs : m_LastResync.healMs;
	}

	int NetMatchService::GetDirectoryVisibility() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_DirectoryRetracted || m_IceBoundSessionId.empty()) return 0;
		return m_DirectoryHidden ? 1 : 2;
	}

	bool NetMatchService::SetDirectoryVisibility(int visibility) {
		if (visibility <= 0) {
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				if (m_DirectoryRetracted || m_IceBoundSessionId.empty()) return true; // already LAN-only
			}
			RetractDirectoryListing();
			return true;
		}
		NetDirectoryRegisterRequest advertised;
		bool running;
		bool listed;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			// A retracted row or a LAN-bound session has no lease to move; relisting is a new session's.
			if (m_DirectoryRetracted || m_IceBoundSessionId.empty()) return false;
			advertised = m_DirectoryRow;
			running = m_State == NetMatchServiceState::Running;
			listed = visibility >= 2;
			if (listed == !m_DirectoryHidden) return true;
			m_DirectoryHidden = !listed;
			m_DirectoryRelistPending = false;
		}
		m_Directory.Advertise(advertised, running, listed);
		m_Directory.Update(SteadyNowMs());
		return true;
	}

	NetMatchService::TransportLink NetMatchService::TakeTransportLinkLocked() {
		DisarmHostLiveness();
		TransportLink link;
		link.migrated = std::move(m_MigratedTransport);
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
		DisarmHostLiveness();
		m_MigratedTransport = std::move(link.migrated);
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
		uint8_t snapshotProvider = 0;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			// Refused before anything is staged, so the running round keeps its coordinator and pump.
			if (!CanResyncLocked(error)) {
				return false;
			}
			isHost = m_IsHost;
			m_FreshRelayRequested = true;
			snapshotProvider = m_Runner ? m_Runner->GetSnapshotProviderPeerId() : 0;
			m_DiagnosticRuntimeError = ScenarioRunner::GetControllerReplayError();
			m_ResyncHealStartMs = SteadyNowMs();
			m_ResyncHealOpen = true;
			// The round owned the transport for the whole match, so nothing stamped a receive while it
			// played. The silence windows start again here instead of measuring the match behind us.
			NotePumpParkedLocked();
			m_HostRepairPending = false;
			m_HostRepairDeferred = false;
		}
		const uint64_t a7Resync = NetA7Journal::BeginResync();
		const bool a7Save = NetA7Journal::Enabled() && isHost && FaultInjected("slow_resync_save");
		const uint64_t a7SaveBegin = a7Save ? AdmissionNowMs() : 0;
		if (a7Save) NetA7Journal::Session("save_begin", a7SaveBegin, {{"resync", std::to_string(a7Resync)}}, "NetMatchService::AdmissionNowMs");
		if (isHost && snapshotProvider == 0 && ClassifyRejoin(g_ActivityMan.GetActivity()) == NetRejoinAnswer::MatchOver) {
			AnswerMatchOverRejoin("match over");
			if (error) {
				*error = "match over";
			}
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_ResyncHealOpen = false;
			return false;
		}
		uint64_t dropFrame = 0;
		if (isHost && snapshotProvider != 0 && m_Coordinator)
			dropFrame = m_Coordinator->GetMigrationResult().boundary + 2;
		if (snapshotProvider != 0 ? snapshotProvider == m_LocalPeerId : isHost) {
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
			{
				std::ostringstream line;
				line << "[net-match] resync snapshot at tick " << savedTick << " (completed " << simTickAtSave << ")";
				System::PrintDiagnosticLine(line.str());
			}
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
			if (!ScenarioRunner::CaptureNetResyncState(dropFrame > 0 ? dropFrame - 1 : 0, state, error)) return false;
			// The host, and only the host, names the checkpoint this match may rewind to: one policy,
			// one answer, carried to every peer instead of each of them choosing for itself. The archive
			// thread proved it restorable when it published it, so the game thread reads no archive here.
			std::optional<AutosaveDescriptor> anchor;
			if (isHost) {
				// Every peer the heal relaunches must hold it: a seat that caught up across a capture never took that one.
				const std::vector<AutosaveDescriptor> validated = AutosaveStore::ValidatedNewestFirst(m_AutosaveMatchId);
				const std::set<uint8_t> relaunched = CheckpointWriters(state.savedTick);
				anchor = ChooseRewindAnchor(validated, m_CheckpointHolders, state.savedTick, relaunched, m_AutosaveIdentity.roundId);
				// The candidates and who reported each, so a heal that names none says why.
				std::string candidates;
				for (const AutosaveDescriptor& checkpoint: validated) {
					candidates += " " + std::to_string(checkpoint.savedTick) + ":";
					if (const auto held = m_CheckpointHolders.find(checkpoint.savedTick); held != m_CheckpointHolders.end())
						for (const uint8_t peer: held->second) candidates += std::to_string(peer);
				}
				System::PrintDiagnosticLine("[autosave] rewind candidates peers=" + std::to_string(relaunched.size()) + candidates);
				if (anchor) {
					state.rewindMatchId = anchor->matchId;
					state.rewindTick = anchor->savedTick;
				}
			} else {
				const auto agreed = GetRewindAnchor();
				state.rewindMatchId = agreed.matchId;
				state.rewindTick = agreed.tick;
			}
			if (!NetResyncCodec::Encode(state, stateBytes, envelope, error)) return false;
			if (isHost && !state.rewindMatchId.empty())
				NoteRewindAnchor(state.rewindMatchId, state.rewindTick, true, anchor ? &*anchor : nullptr);
			const uint64_t archiveBytes = stateBytes.size();
			const uint64_t envelopeBytes = envelope.size();
			const uint64_t saveMs = static_cast<uint64_t>(std::max(0LL, g_ActivityMan.LastSaveMainMs()));
			const uint64_t zipMs = static_cast<uint64_t>(std::max(0LL, g_ActivityMan.LastSaveZipMs()));
			{
				std::ostringstream line;
				line << "[net-match] resync snapshot: archive=" << archiveBytes << " envelope=" << envelopeBytes
			          << " save_ms=" << saveMs << " zip_ms=" << zipMs;
				System::PrintDiagnosticLine(line.str());
			}
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
			m_WorkerSession = session.get();
			NoteSessionHandedToWorker(*session);
			runner = std::move(m_Runner);
			if (isHost) {
				runner->SetStartFrame(ScenarioRunner::ResyncResumeStartFrame(dropFrame));
			}
			if (!m_Coordinator->GetConfig().matchConfig.successorOrder.empty())
				m_MigrationFallbackCoordinator = std::move(m_Coordinator);
			else
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
		// Staging the checkpoint and restarting the activity parked this peer's pump for seconds.
		NotePumpParkedLocked();
		if (!m_ResyncHealOpen) {
			return;
		}
		const uint64_t now = SteadyNowMs();
		m_LastResync.healMs = now >= m_ResyncHealStartMs ? now - m_ResyncHealStartMs : 0;
		m_ResyncHealOpen = false;
		m_LastResync.happened = true;
		m_MigrationFallbackCoordinator.reset();
		m_MigrationRepairPending = false;
		if (m_IsHost && m_Coordinator) {
			const SimCensusScope census;
			m_ReconnectHost.RecordMigrationDepartures(m_Coordinator->GetConfig().startFrame);
		}
		// The moderation the host asked for while the match relaunched lands on the round the relaunch opened, in order.
		if (m_IsHost && m_Session && m_State == NetMatchServiceState::Running && !m_PendingModeration.empty()) {
			std::vector<PendingModeration> pending;
			pending.swap(m_PendingModeration);
			NetKickBanResult issue = NetKickBanResult::Ok;
			for (const PendingModeration& action: pending) {
				const NetKickBanResult result = action.unban ? ApplyUnbanLocked(action.identity) : ApplyRemovalLocked(action.selection, action.action, *m_Session);
				if (issue == NetKickBanResult::Ok) issue = result;
			}
			m_LastKickBanResult = issue;
			System::PrintDiagnosticLine("[net-match] moderation held through the relaunch applied: " + std::to_string(pending.size()) + " action(s)");
		}
	}

	bool NetMatchService::RelaunchInFlightLocked() const {
		if (m_ResyncHealOpen) return true;
		// Between the round's end for a relaunch and the relaunch itself the service still reads as running.
		return m_Coordinator && (m_Coordinator->HasPendingRecoveryStop() ||
		    (!m_Coordinator->IsRunning() && m_Coordinator->GetStats().timeoutReason.starts_with(NetLockstepCodec::StopReasonName(NetLockstepStopReason::ResyncRequested))));
	}

	void NetMatchService::WorkerResyncMain(TransportLink link, NetSession* sessionRaw, NetLockstepCoordinator* coordinatorRaw, NetMatchRunner* runnerRaw, std::vector<uint8_t> stateBytes) {
		std::unique_ptr<NetSession> session(sessionRaw);
		std::unique_ptr<NetLockstepCoordinator> coordinator(coordinatorRaw);
		std::unique_ptr<NetMatchRunner> runner(runnerRaw);
		std::string error;
		{
			// The worker owns this handshake from here: publish it before the work that parks it, or the park and
			// the heartbeats reach the old object while this one counts silence.
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_WorkerSession = session.get();
			SetRejoinPhaseLocked(NetSession::RejoinPhase::ImagePending);
		}
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
		// Staging the snapshot parked this thread for seconds and the game thread cannot reach this
		// session while the worker owns it, so the silence windows start again here.
		session->NotePumpParked();
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
			m_WorkerSession = nullptr;
			m_Coordinator = std::move(coordinator);
			m_Runner = std::move(runner);
			if (!started && !m_IsHost && m_Runner->DidLoseHostDuringSetup() && m_MigrationFallbackCoordinator && m_MigrationFallbackCoordinator->BeginHostMigrationAfterHeal(NetLockstepNowMs())) {
				m_Coordinator = std::move(m_MigrationFallbackCoordinator);
				m_MigrationFallbackReady = true;
				m_State = NetMatchServiceState::Running;
				m_StatusText = "Host lost during repair - arranging handover";
				m_ErrorText.clear();
			} else if (started && !pendingLoad.empty()) {
				m_PendingResyncLoad = pendingLoad;
				m_PendingResyncState = std::move(resyncState);
				m_State = NetMatchServiceState::ReadyToLaunch;
				m_StatusText = "Resynced; relaunching match";
				m_ErrorText.clear();
			} else {
				m_State = NetMatchServiceState::Failed;
				// A resync that died with the host's session is the departure itself, not a resync fault.
				const bool lostHost = !m_IsHost &&
				    (m_Runner->DidLoseHostDuringSetup() || (m_Session && ClientSessionLossIsHostDeparture(*m_Session)));
				m_StatusText = lostHost ? "The host left the match" : "Resync failed";
				m_ErrorText = lostHost ? m_StatusText : error;
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
		// The host named the rewind point; this peer records it and says whether it holds that archive.
		if (!state.rewindMatchId.empty() && !m_IsHost) NoteRewindAnchor(state.rewindMatchId, state.rewindTick, false);
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
		return !m_PendingResyncLoad.empty() || m_PendingAutosaveLoad.has_value();
	}

	std::string NetMatchService::HashSideState(const AutosaveSideState& sideState) {
		return NetIdentity::HashHex(NetIdentity::HashCanonicalText("autosave-sidestate", {{"state", AutosaveStore::RenderSideState(sideState)}}));
	}

	bool NetMatchService::ResumeOfferMatches(const NetLobbyResume& offer, const std::string& worldDigest, const std::string& sideStateHash) {
		// Both halves must be the host's: the same world at that tick, and the same agreed lockstep
		// state to resume it on. An offer missing either names a checkpoint nobody can prove they hold.
		return !offer.digest.empty() && !offer.sideStateHash.empty() &&
		       offer.digest == worldDigest && offer.sideStateHash == sideStateHash;
	}

	bool NetMatchService::AnswerResumeOffer(const NetLobbyResume& offer) {
		if (offer.matchId.empty() || offer.savedTick == 0) return false;
		std::string reason;
		const auto held = AutosaveStore::Find(offer.matchId, offer.savedTick, &reason);
		AutosaveManifest manifest;
		std::string manifestReason;
		const bool hasManifest = held.has_value() &&
		                         AutosaveStore::ReadManifest(held->path.parent_path(), offer.matchId, offer.savedTick, manifest, &manifestReason);
		// The archive must be the very one the host named, and this peer's record of that tick's agreed
		// lockstep state must be the host's, or the two would resume on different state.
		const bool same = hasManifest && ResumeOfferMatches(offer, held->worldStructureHash, HashSideState(manifest.sideState));
		const std::string why = !held ? reason : (!hasManifest ? manifestReason : "the checkpoint's world or agreed state differs");
		{
			std::ostringstream line;
			line << "[autosave] resume offer match=" << offer.matchId << " tick=" << offer.savedTick
		          << (same ? " held locally" : " not held: " + why);
			System::PrintDiagnosticLine(line.str());
		}
		std::unique_lock<std::mutex> lock(m_Mutex, std::defer_lock);
		if (!HoldsServiceLock()) lock.lock();
		m_ResumeHeldMatchId.clear();
		m_ResumeHeldTick = 0;
		m_ResumeHeldRound = 0;
		if (!same) return false;
		m_ResumeHeldMatchId = held->matchId;
		m_ResumeHeldTick = held->savedTick;
		m_ResumeHeldRound = held->roundId;
		m_ResumeHeldSideState = manifest.sideState;
		// The resumed match keeps the checkpoint chain it is resuming, on every peer.
		m_AutosaveMatchId = held->matchId;
		m_AutosaveIdentity.sessionId = held->sessionId;
		m_AutosaveIdentity.roundId = held->roundId;
		m_AutosaveIdentity.intervalSeconds = held->intervalSeconds;
		m_AutosaveIdentity.pinnedCheckpointSource = m_PinnedAutosave;
		m_PinnedAutosave->Store(held->roundId, held->savedTick);
		return true;
	}

	void NetMatchService::StartSnapshotLoadKeepalive() {
		if (m_SnapshotLoadKeepalive.joinable()) return;
		m_SnapshotLoadKeepaliveWindowTicks.store(0);
		m_SnapshotLoadKeepalive = std::jthread([this](std::stop_token stop) {
			while (!stop.stop_requested()) {
				{
					std::lock_guard<std::mutex> lock(m_Mutex);
					// Both pointers are the same handshake: heartbeating only one lets the other time out.
					const uint64_t keepaliveNowMs = AdmissionNowMs();
					if (m_Session) m_Session->TickKeepalive(keepaliveNowMs);
					if (m_WorkerSession && m_WorkerSession != m_Session.get()) m_WorkerSession->TickKeepalive(keepaliveNowMs);
					// Counted under the lock: a load that held it would show a window with no ticks.
					m_SnapshotLoadKeepaliveTicks.fetch_add(1);
					m_SnapshotLoadKeepaliveWindowTicks.fetch_add(1);
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
		});
	}

	void NetMatchService::StopSnapshotLoadKeepalive() {
		if (!m_SnapshotLoadKeepalive.joinable()) return;
		m_SnapshotLoadKeepalive.request_stop();
		m_SnapshotLoadKeepalive.join();
		{
			std::ostringstream line;
			line << "[net-match] snapshot keepalive ticks=" << m_SnapshotLoadKeepaliveWindowTicks.load();
			System::PrintDiagnosticLine(line.str());
		}
	}

	void NetMatchService::ArmHostLivenessLocked() {
		INetTransport* wire = ActiveWireLocked();
		const bool hosting = m_IsHost && wire && m_Coordinator && m_Coordinator->IsRunning() && !m_Coordinator->IsMigrating() &&
		    !m_Coordinator->GetConfig().matchConfig.successorOrder.empty();
		std::vector<uint8_t> bytes;
		std::vector<NetPeerId> targets;
		if (hosting) {
			NetLockstepAck alive;
			alive.senderPeerId = m_Coordinator->GetConfig().localPeerId;
			alive.highestContiguousFrame = m_Coordinator->GetStats().nextFrame;
			if (!NetLockstepCodec::Encode({alive}, bytes)) bytes.clear();
			for (const auto& [peer, transport]: m_Coordinator->RemoteTransports()) targets.push_back(transport);
		}
		const uint64_t nowMs = SteadyNowMs();
		uint64_t sentWhileBusy = 0, busyMs = 0;
		{
			std::lock_guard<std::mutex> lock(m_LivenessMutex);
			sentWhileBusy = m_Liveness.sentWhileBusy;
			busyMs = nowMs >= m_Liveness.pumpMs ? nowMs - m_Liveness.pumpMs : 0;
			m_Liveness.sentWhileBusy = 0;
			m_Liveness.wire = hosting && !bytes.empty() && !targets.empty() ? wire : nullptr;
			m_Liveness.targets = std::move(targets);
			m_Liveness.bytes = std::move(bytes);
			if (hosting) {
				m_Liveness.lane = m_Coordinator->GetConfig().frameLane;
				m_Liveness.tickMs = static_cast<uint64_t>(std::max(1.0, std::ceil(m_Coordinator->GetConfig().simTickMs)));
				m_Liveness.busyLimitMs = m_Coordinator->GetConfig().timeoutMs;
			}
			m_Liveness.pumpMs = nowMs;
		}
		if (sentWhileBusy > 0 && busyMs >= 100) {
			std::ostringstream line;
			line << "[net-match] host liveness: " << sentWhileBusy << " acks from the session thread while the simulation was busy for " << busyMs << "ms";
			System::PrintDiagnosticLine(line.str());
		}
		if (!hosting || m_LivenessThread.joinable()) return;
		m_LivenessThread = std::jthread([this](std::stop_token stop) {
			while (!stop.stop_requested()) {
				{
					std::lock_guard<std::mutex> lock(m_LivenessMutex);
					HostLiveness& live = m_Liveness;
					const uint64_t nowMs = SteadyNowMs();
					// Only while the simulation is busy, from its second missed tick: a running host's own frames and acks already say it is
					// alive. A host stuck past the round's timeout is not busy but hung, and goes quiet.
					if (live.wire && nowMs >= live.pumpMs && nowMs - live.pumpMs >= 2 * live.tickMs && (live.busyLimitMs == 0 || nowMs - live.pumpMs < live.busyLimitMs) &&
					    (nowMs < live.sentMs || nowMs - live.sentMs >= live.tickMs)) {
						for (const NetPeerId target: live.targets) {
							std::string ignored;
							(void)live.wire->Send(target, live.lane, live.bytes, &ignored);
						}
						live.sentMs = nowMs;
						++live.sentWhileBusy;
					}
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(4));
			}
		});
	}

	void NetMatchService::DisarmHostLiveness() {
		std::lock_guard<std::mutex> lock(m_LivenessMutex);
		m_Liveness.wire = nullptr;
		m_Liveness.targets.clear();
	}

	// The load runs on the sim thread without the service lock, so the keepalive must tick right through it.
	bool NetMatchService::RunSnapshotLoadKeepaliveSelfTest(std::string* error) {
		StopSnapshotLoadKeepalive();
		const uint64_t before = m_SnapshotLoadKeepaliveTicks.load();
		StartSnapshotLoadKeepalive();
		const auto start = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(300)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
		const uint64_t during = m_SnapshotLoadKeepaliveTicks.load() - before;
		StopSnapshotLoadKeepalive();
		if (during < 3) {
			if (error) *error = "the snapshot-load keepalive ticked " + std::to_string(during) + " times across a 300 ms load";
			return false;
		}
		{
			std::ostringstream line;
			line << "[net-match-selftest] PASS snapshot_load_keepalive_ticks during_300ms=" << during << " window=" << m_SnapshotLoadKeepaliveWindowTicks.load();
			System::PrintDiagnosticLine(line.str());
		}
		return true;
	}

	bool NetMatchService::StageResyncedMatchLaunch(std::string* error) {
		if (m_WorldCatchUp.active) {
			const std::string pendingLoad = TakePendingResyncLoad();
			if (pendingLoad.empty()) {
				if (error) *error = "no world join snapshot to load";
				return false;
			}
			std::shared_ptr<NetResyncState> committed;
			if (m_WorldCatchUp.privateMatch) {
				committed = std::make_shared<NetResyncState>();
				std::vector<uint8_t> metadata, placeholder;
				if (!ResumeBytes(m_WorldCatchUp.sideState, metadata) || !NetResyncCodec::Decode(metadata, m_WorldCatchUp.checkpointConfig.sessionId,
				    m_WorldCatchUp.snapshotTick + 1, *committed, placeholder, error) || placeholder != std::vector<uint8_t>{0}) return false;
				NetLockstepConfig config;
				config.sessionId = m_WorldCatchUp.checkpointConfig.sessionId;
				config.roundId = m_WorldCatchUp.roundId;
				config.matchConfig = m_WorldCatchUp.checkpointConfig;
				config.peerCount = config.matchConfig.peerCount; config.localPeerId = m_LocalPeerId;
				config.authorityPeerId = m_WorldCatchUp.authorityPeerId;
				config.initialPeerLeaves = m_WorldCatchUp.initialPeerLeaves;
				config.startFrame = m_WorldCatchUp.snapshotTick + 1;
				config.migrationGeneration = m_WorldCatchUp.authorityGeneration;
				config.initialSeatHolds = m_WorldCatchUp.initialHolds;
				config.simTickMs = g_TimerMan.GetDeltaTimeMS();
				m_CatchUpTransport = std::make_unique<LoopbackTransport>();
				m_CatchUpCoordinator = std::make_unique<NetLockstepCoordinator>();
				if (!m_CatchUpCoordinator->StartReplay(*m_CatchUpTransport, config, error)) return false;
				ScenarioRunner::SetLockstepCoordinator(m_CatchUpCoordinator.get());
			} else if (m_Runner && m_Session) {
				// A world's tail replays on the same kind of coordinator, so a seat held in it goes to the AI at its frame.
				NetLockstepConfig config;
				config.sessionId = m_Session->GetSessionId();
				config.roundId = m_WorldCatchUp.roundId;
				config.matchConfig = m_Runner->GetMatchConfig();
				config.peerCount = config.matchConfig.peerCount; config.localPeerId = m_LocalPeerId;
				config.authorityPeerId = m_WorldCatchUp.authorityPeerId;
				SeedWorldReplaySeats(m_WorldCatchUp, config);
				config.startFrame = m_WorldCatchUp.snapshotTick + 1;
				config.migrationGeneration = m_WorldCatchUp.authorityGeneration;
				config.simTickMs = g_TimerMan.GetDeltaTimeMS();
				// A watcher holds no seat: it replays the round as its authority recorded it, and a playback takes no local input.
				if (m_LocalPeerId == 0 || m_LocalPeerId > config.peerCount) config.localPeerId = config.authorityPeerId;
				auto transport = std::make_unique<LoopbackTransport>();
				auto replay = std::make_unique<NetLockstepCoordinator>();
				if (config.peerCount != 0 && config.localPeerId != 0 && config.localPeerId <= config.peerCount && replay->StartReplay(*transport, config, error)) {
					m_CatchUpTransport = std::move(transport);
					m_CatchUpCoordinator = std::move(replay);
					ScenarioRunner::SetLockstepCoordinator(m_CatchUpCoordinator.get());
					// The image's own lockstep state: the handoffs a hold made before its tick are not in the tail that follows it.
					if (!m_WorldCatchUp.sideState.empty()) {
						committed = std::make_shared<NetResyncState>();
						std::vector<uint8_t> metadata, placeholder;
						if (!ResumeBytes(m_WorldCatchUp.sideState, metadata) || !NetResyncCodec::Decode(metadata, config.sessionId, m_WorldCatchUp.snapshotTick + 1, *committed, placeholder, error) ||
						    placeholder != std::vector<uint8_t>{0}) {
							if (error && error->empty()) *error = "the world image's lockstep state does not decode";
							return false;
						}
					}
				}
			}
			const uint64_t stagingBeganMs = SteadyNowMs();
			SetRejoinPhaseLocked(NetSession::RejoinPhase::Loading);
			// A private match's replay coordinator already names the roster; a world's round has none until it starts.
			// The restore's Start consumes it.
			Activity::SetRestoreRoster(m_WorldCatchUp.privateMatch ? nullptr : &m_WorldCatchUp.checkpointConfig, m_LocalPeerId);
			if (!g_ActivityMan.LoadGameToRestart(pendingLoad)) {
				Activity::SetRestoreRoster(nullptr, 0);
				if (error) *error = "world join snapshot load failed: " + pendingLoad;
				return false;
			}
			if (!ScenarioRunner::InstallWorldCatchUp(m_WorldCatchUp.snapshotTick, m_WorldCatchUp.tail, error, m_WorldCatchUp.privateMatch)) {
				return false;
			}
			// A replay run under another peer's id is a watcher's: nothing of this machine's own is held in it.
			ScenarioRunner::SetWorldCatchUpWatcher(m_CatchUpCoordinator && m_CatchUpCoordinator->GetConfig().localPeerId != m_LocalPeerId);
			NoteTailReplayBeganLocked();
			// Loading the snapshot and installing the catch-up own this thread for seconds while nothing reads the
			// session: the admission and silence windows are measured from the end of that work, not across it.
			m_AdmissionClock.NotePark(SteadyNowMs() - stagingBeganMs, SteadyNowMs());
			NotePumpParkedLocked();
			m_WorldCatchUp.tail.clear();
			if (committed && !m_WorldCatchUp.privateMatch) {
				// Restored where the load lands, as a private match's is; the world's seats come from its own roster.
				if (!g_ActivityMan.SetPendingCheckpointCallbacks([] { return true; }, [committed](Activity&) {
					return static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) == committed->savedTick && ScenarioRunner::RestoreCommittedCatchUpState(*committed);
				})) return false;
			} else if (committed) {
				struct LocalState { Activity::NetLocalPlayerState activity; std::string input, gui, frame; bool valid = false, prepared = false; };
				const auto local = std::make_shared<LocalState>();
				const auto pause = m_WorldCatchUp.pauseState;
				m_ActivateCatchUpLocalSeat = [local](Activity& activity) { return local->prepared && activity.Activity::RestoreNetLocalPlayerState(local->activity); };
				if (!g_ActivityMan.SetPendingCheckpointCallbacks([local] {
					local->input = g_UInputMan.SaveCheckpoint(); local->gui = GUIInput::SaveSharedCheckpoint(); local->frame = g_FrameMan.SaveNetLocalState();
					if (g_ActivityMan.GetActivity()) local->valid = g_ActivityMan.GetActivity()->CaptureNetLocalPlayerState(local->activity);
					return true;
				}, [local, committed, pause, localPeer = m_LocalPeerId](Activity& activity) {
					if (static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) != committed->savedTick) return false;
					if (local->valid) {
						if (!activity.RestoreNetLocalPlayerState(local->activity) || !activity.Activity::ApplyNetPlayerBindings(NetGamePlayerBindings{})) return false;
						local->prepared = true;
					} else {
						// A relaunched process has no player state of its own: its seats are the committed world's, mapped to this machine.
						const auto roster = ScenarioRunner::GetLockstepMatchConfig();
						if (roster ? !activity.AdoptNetLocalSeat(*roster, localPeer) : !activity.ApplyNetPlayerBindings(NetGamePlayerBindings{})) return false;
					}
					return g_UInputMan.LoadCheckpoint(local->input) && GUIInput::LoadSharedCheckpoint(local->gui) && g_FrameMan.LoadNetLocalState(local->frame) &&
					    ScenarioRunner::RestoreCommittedCatchUpState(*committed) && ScenarioRunner::RestoreLockstepPauseState(pause, committed->savedTick);
				})) return false;
				g_ActivityMan.NoteLockstepRelaunch();
			}
			ScenarioRunner::ApplyDeterministicConfig();
			return true;
		}
		std::optional<PendingAutosaveLoad> autosave;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			autosave = m_PendingAutosaveLoad;
			m_PendingAutosaveLoad.reset();
		}
		const std::string pendingLoad = TakePendingResyncLoad();
		if ((pendingLoad.empty() && !autosave) || !m_PendingResyncState) {
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
		// A resumed peer launches on the carried bindings, so it needs its own entry whatever it kept.
		if ((!retainLocal || autosave) && !newestBinding && !dedicated) {
			if (error) *error = "the resync snapshot has no player bindings for this peer";
			return false;
		}
		// A resync worker's seat loads the snapshot with nobody to answer, and plays live the moment it lands.
		const NetSession* rejoining = m_Session ? m_Session.get() : m_WorkerSession;
		const bool resyncRejoin = rejoining && rejoining->GetRejoinPhase() != NetSession::RejoinPhase::Active;
		if (resyncRejoin) SetRejoinPhaseLocked(NetSession::RejoinPhase::Loading);
		StartSnapshotLoadKeepalive();
		if (autosave) {
			if (!g_ActivityMan.LoadAutosaveToRestart(autosave->matchId, autosave->tick)) {
				StopSnapshotLoadKeepalive();
				if (error) *error = "checkpoint load failed: " + AutosaveStore::ArchiveName(autosave->matchId, autosave->tick);
				return false;
			}
		} else if (!g_ActivityMan.LoadGameToRestart(pendingLoad)) {
			StopSnapshotLoadKeepalive();
			if (error) *error = "resync snapshot load failed: " + pendingLoad;
			return false;
		}
		g_ActivityMan.NoteLockstepRelaunch();
		// This peer's own checkpoint of that tick already carries its own local player state, so the
		// staging keeps the world's own bindings instead of applying one derived from the roster.
		const bool ownCheckpoint = autosave.has_value();
		if (ownCheckpoint) {
			{
				std::ostringstream line;
				line << "[net-match] launching from the held checkpoint: "
			          << AutosaveStore::ArchiveName(autosave->matchId, autosave->tick);
				System::PrintDiagnosticLine(line.str());
			}
		} else {
			const char* keepResyncSaves = std::getenv("CC_KEEP_RESYNC_SAVES");
			if (keepResyncSaves && keepResyncSaves[0] && keepResyncSaves[0] != '0') {
				{
					std::ostringstream line;
					line << "[net-match] keeping resync save: " << pendingLoad;
					System::PrintDiagnosticLine(line.str());
				}
			} else {
				g_ActivityMan.RemoveSavedGame(pendingLoad);
			}
			{
				std::ostringstream line;
				line << "[net-match] launching from the received snapshot: " << pendingLoad;
				System::PrintDiagnosticLine(line.str());
			}
		}
		struct LocalState { Activity::NetLocalPlayerState activity; std::string input, gui, frame; };
		const auto local = std::make_shared<LocalState>();
		const bool keepLocalPlayer = retainLocal && !dedicated;
		if (!g_ActivityMan.SetPendingCheckpointCallbacks([local, keepLocalPlayer] {
			local->input = g_UInputMan.SaveCheckpoint();
			local->gui = GUIInput::SaveSharedCheckpoint();
			local->frame = g_FrameMan.SaveNetLocalState();
			return !keepLocalPlayer || (g_ActivityMan.GetActivity() && g_ActivityMan.GetActivity()->CaptureNetLocalPlayerState(local->activity));
		}, [this, local, state, keepLocalPlayer, dedicated, newestBinding, ownCheckpoint](Activity& activity) {
			StopSnapshotLoadKeepalive();
			// Each refusal names its step: the relaunch reports only that the restart failed.
			const auto refuse = [](const char* step) { System::PrintDiagnosticLine(std::string("[net-match] resync restore refused: ") + step); return false; };
			if (static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) != state->savedTick) return refuse("the restored tick is not the snapshot's");
			if (!g_UInputMan.LoadCheckpoint(local->input, true) || !GUIInput::LoadSharedCheckpoint(local->gui, true) || !g_FrameMan.LoadNetLocalState(local->frame, true))
				return refuse("this machine's input, GUI or frame state does not load");
			const NetGamePlayerBindings seatless{};
			// A peer that loaded its own copy of the checkpoint and one that was streamed the host's copy
			// apply the SAME bindings - the ones the checkpoint's manifest carried - or the two would
			// resume onto different seats. Only a live heal keeps this machine's own captured state.
			if (!(keepLocalPlayer && !ownCheckpoint ? activity.RestoreNetLocalPlayerState(local->activity)
			                                        : activity.ApplyNetPlayerBindings(dedicated ? seatless : newestBinding->bindings))) {
				return refuse(keepLocalPlayer && !ownCheckpoint ? "this machine's local player state does not restore" : "the snapshot's player bindings do not apply");
			}
			if (!g_UInputMan.LoadCheckpoint(local->input) || !GUIInput::LoadSharedCheckpoint(local->gui) || !g_FrameMan.LoadNetLocalState(local->frame))
				return refuse("this machine's input, GUI or frame state does not apply");
			return ScenarioRunner::RestoreNetResyncState(*state) || refuse("the lockstep resync state does not restore");
		})) { StopSnapshotLoadKeepalive(); if (error) *error = "could not stage resync local state restoration"; return false; }
		ScenarioRunner::ApplyDeterministicConfig();
		if (resyncRejoin) SetRejoinPhaseLocked(NetSession::RejoinPhase::Active);
		return true;
	}

	// Same shape as WorkerMain: the objects live as worker locals while the lobby round runs, so
	// report/snapshot readers never race a mid-mutation runner; they move back in when it settles.
	void NetMatchService::WorkerRematchMain(TransportLink link, NetSession* sessionRaw, NetLockstepCoordinator* coordinatorRaw, NetMatchRunner* runnerRaw, bool departedHost) {
		std::unique_ptr<NetSession> session(sessionRaw);
		std::unique_ptr<NetLockstepCoordinator> coordinator(coordinatorRaw);
		std::unique_ptr<NetMatchRunner> runner(runnerRaw);
		std::string error;
		const bool started = runner->StartNextMatch(*link.Wire(), *session, *coordinator, &error, {}, link.lobbyEvents);
		const bool optionsRefused = !started && runner->HasRefusedHostOptions();
		if (!optionsRefused) link.lobbyEvents.clear();
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
				if (m_IsHost) {
					m_PendingRoundStartScripts = m_RoundStartScripts;
				} else if (std::vector<uint8_t> received = runner->TakeReceivedState(); IsRoundStartScriptBlob(received)) {
					m_PendingRoundStartScripts = std::move(received);
				}
			}
			RestoreTransportLinkLocked(std::move(link));
			m_Session = std::move(session);
			m_WorkerSession = nullptr;
			m_Coordinator = std::move(coordinator);
			m_Runner = std::move(runner);
			m_RematchReturnOwed = false;
			if (started) {
				m_State = NetMatchServiceState::ReadyToLaunch;
				m_StatusText = "Ready to launch match";
				m_ErrorText.clear();
			} else if (optionsRefused) {
				m_State = NetMatchServiceState::Completed;
				m_StatusText = "Correct the host options before starting the rematch";
				m_ErrorText = error;
				m_AdoptedMatchConfig = m_Runner->GetMatchConfig();
			} else {
				m_State = NetMatchServiceState::Failed;
				{
					std::ostringstream line;
					line << "[net-match] rematch setup failed: " << error;
					System::PrintDiagnosticLine(line.str());
				}
				const bool missingPlayers = error == "rematch roster: not enough players for a rematch";
				const bool lostHost = m_Runner->DidLoseHostDuringSetup() || (departedHost && missingPlayers) || m_HostEndedTheMatch;
				if (lostHost && departedHost && !m_IsHost) NoteHostEndedTheMatchLocked();
				m_RematchReturnOwed = RematchLossReturnsThroughRejoin(m_IsHost, m_HostEndedTheMatch, error.starts_with("rematch roster"), m_Session && m_Session->IsReady(),
				                                                      m_Session && m_Session->HasReject(), m_Session && m_Session->HasReject() ? m_Session->GetRejectReason() : NetRejectReason::InternalError,
				                                                      m_Runner->DidLoseHostDuringSetup(), error == "timed out waiting for lockstep start",
				                                                      error == NetMatchRunner::c_SeatStartsHeld);
				m_ErrorText = lostHost ? "The host left the match" :
				              missingPlayers ? "The other players left the match" :
				              error.starts_with("rematch roster") ? "The match could not return to the lobby" : error;
				m_StatusText = lostHost || missingPlayers ? m_ErrorText : "Rematch setup failed";
			}
			m_WorkerDone = true;
		}
	}

	void NetMatchService::ResetRosterTransitionHistory() {
		m_RosterTransitions.clear();
		m_RosterTransitionsDropped = 0;
		m_LastRosterPair.clear();
	}

	void NetMatchService::RecordRosterTransitions(const std::map<uint8_t, SeatView>& previous, uint64_t observedAtMs) {
		UpdateSummarySeatsLocked();
		const uint64_t appliedFrame = ScenarioRunner::GetLockstepAppliedFrame();
		for (const auto& [peerId, view]: m_SeatViews) {
			std::pair<std::string, std::string>& last = m_LastRosterPair[peerId];
			const auto before = previous.find(peerId);
			const bool known = before != previous.end();
			const bool newHolder = known && before->second.seat.owner != 0 && view.seat.owner != 0 && view.seat.owner != before->second.seat.owner;
			// Another seat on this slot (a world's promoted watcher, or a promotion undone): a player taking it joins, and the one
			// it moves from is named under its own seat's view.
			const bool otherSeat = known && before->second.stableSeat != view.stableSeat;
			if (last.first == view.state && last.second == view.line && !newHolder && !otherSeat) continue;
			const bool firstSeen = last.first.empty();
			const bool wasAway = known && SeatViewAway(before->second);
			// A seat the host opened has no holder left to name: the player who had it is the one who went.
			const std::string& who = view.seat.owner == 0 && known ? before->second.name : view.name;
			last = {view.state, view.line};
			if (otherSeat) {
				if (view.seat.owner != 0 && peerId != m_LocalPeerId) ScenarioRunner::PushNetUiToast("player_joined", who + " joined");
			} else if (firstSeen && view.state == "Present" && peerId != m_LocalPeerId) {
				ScenarioRunner::PushNetUiToast("player_joined", who + " joined");
			} else if (newHolder) {
				ScenarioRunner::PushNetUiToast("player_substituted", who + " joined as substitute");
			} else if (view.seat.owner == 0 && known && before->second.seat.owner != 0) {
				ScenarioRunner::PushNetUiToast("player_left", who + " left");
			} else if (SeatViewAway(view) && !firstSeen && !wasAway) {
				// A seat the round holds in place keeps its player; a link that went is a drop, and a leave is said as one.
				const bool left = view.seat.holdCause == NetSeatHoldCause::Leave;
				ScenarioRunner::PushNetUiToast(left ? "player_left" : "player_dropped", who + (left ? " left" : " dropped"));
			} else if (view.state == "Present" && wasAway) {
				ScenarioRunner::PushNetUiToast("player_rejoined", who + " rejoined");
			}
			if (m_RosterTransitions.size() >= 256) {
				++m_RosterTransitionsDropped;
				continue;
			}
			m_RosterTransitions.push_back({peerId, view.state, view.line, appliedFrame, observedAtMs});
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
		m_SeatViews.clear();
		ResetRosterTransitionHistory();
		m_ModerationSeats.clear();
		m_LobbyModerationSignature = 0;
		m_LobbyModerationPublished = false;
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
		System::PrintDiagnosticLine(std::string("[net-reconnect] leave: ") + NetReconnectClientStateName(m_ReconnectClient.GetState()) +
		                            (m_TicketStore.HasRecord() ? " (ticket kept)" : " (ticket cleared)"));
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
		DisarmHostLiveness();
		if (m_LivenessThread.joinable()) { m_LivenessThread.request_stop(); m_LivenessThread.join(); }
		StopSnapshotLoadKeepalive();
		if (m_Worker.joinable()) {
			m_Worker.join();
		}
		m_IdentityPending = false;
		m_LeftRoundHeld = false;
		SealPendingWorldSegmentAtEnd();
		// A host whose match went on under a successor writes nothing of its own play and leaves the listing to that successor.
		const bool superseded = m_IsHost && m_Coordinator && m_Coordinator->IsSuperseded();
		// A restart imports the seats as the round left them: a clean leave in its last seconds releases its seat there too.
		if (!superseded) PublishRestartAdmission();
		// A clean stop of a world leaves the tick it stopped on, before anything is torn down.
		WriteFinalWorldCheckpoint();
		RunCleanLeave();
		// The round is over here too: an admission file with no checkpoint left behind it goes now.
		SweepRestartAdmission();
		m_LanDiscovery.Stop();
		if (superseded)
			m_Directory.AbandonLease();
		else
			m_Directory.Shutdown(); // the DELETE goes out before the row would expire
		ReleaseHostPortMap();   // the router mapping goes out with the listing
		ScenarioRunner::SetLockstepCoordinator(nullptr);
		ScenarioRunner::SetSessionPump(nullptr);
		std::unique_ptr<NetMatchRunner> runner;
		m_CatchUpCoordinator.reset(); m_CatchUpTransport.reset();
		m_ActivateCatchUpLocalSeat = {};
		m_InPlaceCatchUp = false; m_InPlaceIncarnationBumps.clear(); m_RejoinFitReasons.clear(); m_CommittedRing.Clear(); m_CommittedRingRound = 0; m_CommittedRingGeneration = 0;
		m_HandoverFrame = 0; m_InPlaceRoutes.clear(); m_InPlaceMoveHost = 0; m_InPlaceMoveAddress.clear(); m_InPlaceTicketHost.clear();
		m_PrivateBaseRequested = false; m_PrivateBaseTick = 0; m_PrivateBasePending.reset();
		m_PrivateImageTask = {}; m_PrivateImageRound = 0; m_PrivateImageStaleFrom = 0; m_PrivateImageSeatHeld = false; m_PrivateImageTakenMs = 0; m_PrivateImageLastCaptureMs = 0.0; m_PrivateCaptureCosts.clear(); m_PrivateCaptureCold = false; m_PrivateJoinError.clear();
		m_WorldJoin.Reset(); m_WorldCatchUp = {};
		m_LastJoinRoute.reset();
		m_WorldCaptureRequestedTick = 0;
		m_WorldWatcherSeatedAt = 0;
		m_WorldCapturePending = false;
		ResetCheckpointSchedule();
		m_PrivateActivations.clear(); m_PrivateJoinBlobs.clear(); m_CatchUpWirePackets.clear(); m_CatchUpWireBytes = 0;
		ScenarioRunner::ReleaseWorldCatchUp();
		m_WorldJoinImageArchive.reset(); m_WorldJoinImageDigest.clear();
		std::unique_ptr<NetLockstepCoordinator> coordinator;
		std::unique_ptr<NetSession> session;
		std::unique_ptr<GnsTransport> transport;
		std::unique_ptr<NetMuxTransport> mux;
		std::unique_ptr<INetTransport> migrated;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			DisarmHostLiveness();
			if (m_Mux) m_Mux->SetPump({});
#ifdef CCCP_WITH_GNS
			if (m_Dispatcher) {
				m_IceReport = m_Dispatcher->BuildReportJson();
				m_Dispatcher->Stop();
				m_Dispatcher.reset();
			}
#endif
			mux = std::move(m_Mux);
			migrated = std::move(m_MigratedTransport);
			m_MigrationFallbackCoordinator.reset();
			m_MigrationFallbackReady = false;
			m_MigrationRepairPending = false;
			m_MigrationKey.fill(0);
			m_MigrationAdmissionState.clear();
			m_LastMigrationAdmissionState.clear();
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
			m_KeepEndedDirectoryLease = false;
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
			// A staged options draft names the session it was accepted under; teardown drops it
			// with that config so the next lobby never sees its predecessor's edit.
			m_AdoptedMatchConfig = {};
			m_PendingHostOptions.reset();
			m_HostOptionsRequest.Clear();
			m_ResyncOnDesync = false;
			m_ResyncHealOpen = false;
			m_HostRepairPending = false;
			m_HostRepairDeferred = false;
			m_ResyncHealStartMs = 0;
			m_LastResync = {};
			m_PendingHeldReseats.clear();
			m_PendingHeldResolutions.clear();
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
			m_LandedWithoutFrame = false;
			m_FailedWithoutFrame = false;
			m_RejoinOfRunningMatch = false;
			m_HostEndedTheMatch = false;
			m_LeftMatch = false;
			m_HostEndReason.clear();
			m_CompletedLobbySinceMs = 0;
			m_PendingLobbyEvents.clear();
			m_PendingLobbyBytes = 0;
			m_PendingLobbyOverflow = false;
			m_EndedLockstepPackets = 0;
			m_JoinRefusalKey.clear();
			m_JoinRefusalSeat.reset();
			ResetRoundGoodbyeLocked();
			EndAdmissionSession();
		}
		runner.reset();
		coordinator.reset();
		session.reset();
		migrated.reset();
		mux.reset();
		transport.reset();
	}

	void NetMatchService::ReportRuntimeError(const std::string& error) {
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_DiagnosticRuntimeError = error;
			m_HeldRejoinDriving = false;
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
		std::unique_ptr<INetTransport> migrated;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			DisarmHostLiveness();
			if (m_Mux) m_Mux->SetPump({});
#ifdef CCCP_WITH_GNS
			if (m_Dispatcher) {
				m_IceReport = m_Dispatcher->BuildReportJson();
				m_Dispatcher->Stop();
				m_Dispatcher.reset();
			}
#endif
			mux = std::move(m_Mux);
			migrated = std::move(m_MigratedTransport);
			m_MigrationFallbackCoordinator.reset();
			m_MigrationFallbackReady = false;
			m_MigrationRepairPending = false;
			DrainPendingSessionEventsLocked(false);
			AccumulateLockstepTotalsLocked();
			if (m_CapturedRunnerReport.empty() && m_Runner && m_Session && m_Coordinator) {
				m_CapturedRunnerReport = m_Runner->BuildReportJson(*m_Session, *m_Coordinator);
			}
			// Only the host's departure lands a seat that committed nothing; an error on this peer's own side keeps
			// its seat's reconnect, so a held client retries its private rejoin.
			const bool hostDeparted = !m_IsHost && ((m_Runner && m_Runner->DidLoseHostDuringSetup()) || (m_Session && ClientSessionLossIsHostDeparture(*m_Session)));
			// A seat rejoining the match it played has committed frames, whatever the fresh round it was rejoining has not.
			m_FailedWithoutFrame = hostDeparted && !m_RejoinOfRunningMatch && m_Coordinator && !m_Coordinator->HasCompletedSimulationTick();
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
			// A worker that already named the host's departure keeps that verdict; the caller's bare
			// pump error would demote it back to a generic fault.
			if (m_ErrorText == "The host left the match") {
				m_StatusText = m_ErrorText;
			} else {
				m_StatusText = "Match stopped";
				m_ErrorText = error;
			}
			m_LobbySnapshot = {};
			m_InputDelayText.clear();
			EndAdmissionSession();
		}
		runner.reset();
		coordinator.reset();
		session.reset();
		migrated.reset();
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
		if (m_KeepEndedDirectoryLease)
			return m_IsHost && !m_DirectoryRetracted && m_DirectoryRegistered;
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
		bool heldSeatNeedsAnswer = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			CaptureMatchSummaryLocked(reason);
			if (m_LastMatchSummary) displayReason = m_LastMatchSummary->result;
			heldSeatNeedsAnswer = m_IsHost && m_Coordinator && m_Coordinator->AnyHeldAISeat();
			if (heldSeatNeedsAnswer) {
				m_KeepEndedDirectoryLease = true;
				m_ReconnectHost.SetMatchEnded();
			}
		}
		// The recording gets its end marker at the match's end, not at process exit.
		SealPendingWorldSegmentAtEnd();
		ScenarioRunner::CloseLockstepReplayRecord();
		if (heldSeatNeedsAnswer || ShouldKeepIceDirectoryLease()) {
			HideDirectoryListing();
		} else {
			RetractDirectoryListing();
		}
		std::lock_guard<std::mutex> lock(m_Mutex);
		DrainPendingSessionEventsLocked(false);
		if (m_Coordinator) {
			m_Coordinator->Complete(reason);
			SayGoodbyeToRejoinersLocked();
		}
		if (m_State == NetMatchServiceState::Running) {
			m_StatusText = displayReason.empty() ? "Match complete" : displayReason;
			m_ErrorText.clear();
		}
	}

	bool NetMatchService::EndMatchAtAgreedFrame(const std::string& reason) {
		// The pause menu calls this inside the frame's window, as it does FinishMatch.
		NetLockstepPlane::Gap plane("end match");
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_IsHost || !m_Coordinator || m_State != NetMatchServiceState::Running || !m_Coordinator->IsRunning()) return false;
		DrainPendingSessionEventsLocked(false);
		if (!m_Coordinator->CompleteAtAgreedEnd(reason)) return false;
		m_HostEndReason = reason;
		return true;
	}

	bool NetMatchService::EndsAtAgreedFrame() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return !m_HostEndReason.empty();
	}

	// Terminal clean end; the session objects stay alive for the next Start or quit.
	void NetMatchService::FinishMatch(const std::string& result) {
		// The frame's menus call this inside the plane's window, and it reads the round without the plane's lock.
		NetLockstepPlane::Gap plane("finish match");
		std::string displayResult = result;
		bool heldSeatNeedsAnswer = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			// A host's round that played to its agreed end finishes with the reason the host gave.
			const std::string ended = m_HostEndReason.empty() ? result : std::exchange(m_HostEndReason, std::string());
			displayResult = ended;
			CaptureMatchSummaryLocked(ended);
			if (m_LastMatchSummary) displayResult = m_LastMatchSummary->result;
			m_HeldRejoinDriving = false;
			heldSeatNeedsAnswer = m_IsHost && m_Coordinator && m_Coordinator->AnyHeldAISeat();
			if (m_IsHost) m_ReconnectHost.SetMatchEnded();
			// The round ended because its host announced its leave: that is the host's end of the match for this seat.
			if (!m_IsHost && m_Coordinator && m_Coordinator->GetPeerLeaveFrames().contains(m_Coordinator->GetHostPeerId())) NoteHostEndedTheMatchLocked();
			if (heldSeatNeedsAnswer) m_KeepEndedDirectoryLease = true;
			DrainPendingSessionEventsLocked(false);
			// The survivors hear the end before anything here waits on a disk: the last checkpoint's archive may still be writing.
			if (m_Coordinator) {
				m_Coordinator->Complete(ended.empty() ? "match over" : ended);
				SayGoodbyeToRejoinersLocked();
			}
		}
		SealPendingWorldSegmentAtEnd();
		ScenarioRunner::SetLockstepCoordinator(nullptr);
		ScenarioRunner::ResetRetiredChecksumCounters();
		ScenarioRunner::SetSessionPump(nullptr);
		if (heldSeatNeedsAnswer || ShouldKeepIceDirectoryLease()) {
			HideDirectoryListing();
		} else {
			RetractDirectoryListing();
		}
		std::lock_guard<std::mutex> lock(m_Mutex);
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
		bool handover = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			CaptureMatchSummaryLocked(result);
			handover = m_IsHost && m_Coordinator && m_Coordinator->IsRunning() && !m_Coordinator->GetConfig().matchConfig.successorOrder.empty();
			if (m_LastMatchSummary) displayResult = m_LastMatchSummary->result;
			if (m_IsHost) m_ReconnectHost.SetMatchEnded();
			m_LeftMatch = true;
			DrainPendingSessionEventsLocked(false);
		}
		SealPendingWorldSegmentAtEnd();
		ScenarioRunner::SetLockstepCoordinator(nullptr);
		ScenarioRunner::SetSessionPump(nullptr);
		if (handover)
			m_Directory.AbandonLease();
		else
			RetractDirectoryListing();
		const std::string reason = result.empty() ? std::string("player left") : result;
		bool exchangeOwed = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			// The round hears the leave at once: this seat stopped producing here, and a leave that waited on §7's
			// ack would be held as a slow player first. The notice leaves the link up, so §7 still runs on it.
			exchangeOwed = !m_LeaveExchangeRun && m_AdmissionAttached && !m_IsHost && m_Session &&
			               m_Session->IsReady() && m_TicketStore.HasRecord();
			if (m_Coordinator) {
				m_Coordinator->Leave(reason);
			}
			if (m_State == NetMatchServiceState::Running) {
				m_State = NetMatchServiceState::Completed;
				m_StatusText = displayResult.empty() ? "Left the match" : displayResult;
				m_ErrorText.clear();
			}
			// The seat stays this player's while the match runs, so the landing offers Rejoin Match at once.
			if (!m_IsHost && m_MatchWasRunning && m_TicketStore.HasRecord()) ScanStoredTicket();
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
		if (m_MigrationDirectoryResumePending) {
			m_Directory.Configure(g_SettingsMan.GetSessionDirectoryUrl(), g_SettingsMan.GetOrCreateSessionDirectoryInstallKey(), g_SettingsMan.GetSessionDirectoryCertSha256());
			// The successor claims the row at the generation it hosts: the directory takes one claim per generation.
			m_DirectoryRow.migrationGen = static_cast<int64_t>(m_MigrationGeneration);
			(void)m_Directory.Resume(m_DirectoryRow, m_MigrationDirectorySession, m_MigrationDirectoryToken);
			m_MigrationDirectoryResumePending = false;
		}
		if (m_MigrationFallbackReady) {
			m_Coordinator->Tick(NetLockstepNowMs());
			PumpHostMigration();
			if (m_Coordinator->IsFailed() && m_Session->IsReady()) {
				m_MigrationFallbackReady = false;
				ScenarioRunner::SetLockstepCoordinator(m_Coordinator.get(), true);
				std::string error;
				if (!ResyncMatch(&error))
					SetState(NetMatchServiceState::Failed, "Host handover repair failed", error);
			} else if (m_Coordinator->IsStopped()) {
				m_MigrationFallbackReady = false;
				SetState(NetMatchServiceState::Failed, "Host handover ended", m_Coordinator->GetStats().timeoutReason);
			}
		}
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			PumpCompletedSessionLocked();
		}
		SettleKeptDirectoryLease();
		// A hosting lobby advertises itself on the LAN until the match launches.
		bool beaconWanted = false;
		bool directoryWanted = false;
		bool mappingKept = false;
		bool directoryRunning = false;
		bool directoryListed = true;
		int64_t directorySeatsFree = 0;
		int64_t directorySeatsHeld = 0;
		NetLobbySnapshot snapshot;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			beaconWanted = m_IsHost && !m_IdentityPending && m_State == NetMatchServiceState::Starting;
			if (beaconWanted) {
				snapshot = m_LobbySnapshot;
			}
			// A listing that waits only for the lobby's identity is still wanted: its router mapping stays.
			mappingKept = m_IsHost && !m_DirectoryRetracted &&
			              (m_State == NetMatchServiceState::Starting || m_State == NetMatchServiceState::ReadyToLaunch ||
			               m_State == NetMatchServiceState::Running || (m_DirectoryHidden && m_State == NetMatchServiceState::Completed));
			directoryWanted = mappingKept && !m_IdentityPending;
			directoryRunning = m_State == NetMatchServiceState::Running;
			directoryListed = !m_DirectoryHidden;
			if (directoryWanted) {
				if (directoryRunning && !m_SeatStatuses.empty()) {
					// The admission table says which seats a late joiner could still take: the host's
					// own seat and the CPU slot never count, a committed or closed one is taken.
					for (const NetH4SeatStatus& seat : m_SeatStatuses) {
						// A world's watcher seats are counted as watchers, not as gameplay seats.
						if (seat.lockstepPeerId >= c_WorldSpectatorLobbyPeerFirst) {
							continue;
						}
						if (NetH4SeatIsOpen(seat.lockstepPeerId, m_LocalPeerId, seat.committed, seat.closed)) {
							++directorySeatsFree;
						}
						if (NetH4SeatIsHeld(seat.lockstepPeerId, m_LocalPeerId, seat.held, seat.closed)) {
							++directorySeatsHeld;
						}
					}
				} else if (!m_LobbySnapshot.members.empty()) {
					// The roster lists every configured slot, so an open seat is a non-CPU slot no
					// peer has connected into yet.
					for (const NetLobbyMember& member : m_LobbySnapshot.members) {
						if (!member.cpu && !member.connected && !member.dropped && !member.reclaiming) {
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
			                                 static_cast<uint8_t>(snapshot.members.empty() ? (m_Dedicated ? 0 : 1) :
			                                     std::count_if(snapshot.members.begin(), snapshot.members.end(), [&](const NetLobbyMember& member) {
				                                     return !member.cpu && !(m_Dedicated && member.isLocal) &&
				                                            (member.connected || member.dropped || member.reclaiming);
			                                     })),
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
		m_Directory.Configure(directoryUrl, directoryKey, directoryCertPin);
		// The row registers at once with the addresses it has; a mapping that answers later reaches it on the next heartbeat.
		if (directoryWanted) {
			NetDirectoryRegisterRequest advertised;
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_DirectoryRow.peerCount = m_BeaconMaxPlayers;
				m_DirectoryRow.seatsFree = directorySeatsFree;
				m_DirectoryRow.seatsHeld = directorySeatsHeld;
				m_DirectoryRow.migrationGen = static_cast<int64_t>(m_MigrationGeneration);
				m_DirectoryRow.spectatorFree = m_WorldSpectatorsFree;
				m_DirectoryRow.joinMode = NetIceRowJoinMode(m_IceEnabled, !m_DirectoryRow.listenAddrs.empty(), m_IceBoundSessionId, m_Directory.GetSessionId());
				advertised = m_DirectoryRow;
				// A register receives a new id; only the existing bound row can advertise ICE.
				advertised.joinMode = NetIceRowJoinMode(m_IceEnabled, !advertised.listenAddrs.empty(), m_IceBoundSessionId, std::string());
			}
			if (!advertised.resumeSessionId.empty() && m_Directory.GetState() == NetDirectoryClient::State::Idle && m_Directory.GetSessionId().empty()) {
				(void)m_Directory.Resume(advertised, advertised.resumeSessionId, advertised.resumeToken, directoryRunning, directoryListed);
			} else {
				m_Directory.Advertise(advertised, directoryRunning, directoryListed);
			}
		} else {
			m_Directory.Retract();
			// The mapping goes out with a retracted listing or the end of hosting, and nothing is left pending after it.
			if (s_PortMapRequested && !mappingKept) ReleaseHostPortMap();
		}
		// A provisional host refreshes no listing: the match may have gone on under the next generation.
		if (!m_IsHost || !m_Coordinator || !m_Coordinator->IsHostProvisional()) m_Directory.Update(nowMs);
		// The directory holds the row at a later generation: the match went on without this host.
		if (m_IsHost && m_Coordinator && m_Directory.GetState() == NetDirectoryClient::State::Superseded) m_Coordinator->NoteSuperseded(static_cast<uint64_t>(m_Directory.GetSupersededGeneration()));
		UpdateRelayOffer(nowMs);
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
		// The host's restart admission rides this pump: file IO, never the sim thread.
		PublishRestartAdmission();
		SweepRestartAdmission();
		// 7e: while the prompt waits for a host to come back, this is what watches for its row.
		PumpHostReturnWatch(nowMs);
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
			// The rematch seating wait is the host's own idle policy, the same one the setup lobby
			// announced; Never (0) leaves the lobby open until it seats or the host closes it.
			expired = m_AdoptedMatchConfig.idleWaitMinutes > 0 &&
			          nowMs >= m_CompletedLobbySinceMs + static_cast<uint64_t>(m_AdoptedMatchConfig.idleWaitMinutes) * 60000;
			if (expired) {
				m_CompletedLobbySinceMs = 0;
			}
		}
		if (!expired) {
			return;
		}
		{
			std::ostringstream line;
			line << "[net-match] rematch lobby expired after the host's idle wait";
			System::PrintDiagnosticLine(line.str());
		}
		Destroy();
		SetState(NetMatchServiceState::Idle, "Idle", "The rematch lobby timed out.");
	}

	uint64_t NetMatchService::AdmissionNowMs() const {
		return m_AdmissionClock.NowMs(SteadyNowMs());
	}

	NetJoinRefusalOffer NetMatchService::JoinRefusalOffer(uint16_t* ownSeat) const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_State != NetMatchServiceState::Failed) return NetJoinRefusalOffer::None;
		const NetJoinRefusalOffer offer = NetJoinRefusalOfferOf(m_JoinRefusalKey);
		if (offer == NetJoinRefusalOffer::OwnSeat && !m_JoinRefusalSeat) return NetJoinRefusalOffer::None;
		if (ownSeat && m_JoinRefusalSeat) *ownSeat = *m_JoinRefusalSeat;
		return offer;
	}

	bool NetMatchService::BeginSeatApplication(const NetMatchServiceRequest& request, uint16_t stableSeat, std::string* error) {
		s_ApplyOnce = true;
		s_ApplyOnceSeat = stableSeat;
		if (Start(request, error)) return true;
		s_ApplyOnce = false;
		s_ApplyOnceSeat = c_NetH4AnySubstitutableSeat;
		return false;
	}

	bool NetMatchService::BeginSlotWait(const NetMatchServiceRequest& request, std::string* error) {
		s_WaitForSlotOnce = true;
		if (Start(request, error)) return true;
		s_WaitForSlotOnce = false;
		return false;
	}

	bool NetMatchService::BeginSubstituteApplication(const NetMatchServiceRequest& request, std::string* error) {
		s_ApplyOnce = true;
		s_ApplyOnceSeat = c_NetH4AnySubstitutableSeat;
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

	void NetMatchService::NoteHostEndedTheMatchLocked() {
		m_HostEndedTheMatch = true;
		// The host's own end of its match is the confirmed end the ticket waits for: there is no seat left to reclaim.
		if (m_TicketStore.HasRecord()) (void)m_TicketStore.Clear(nullptr);
		m_ReconnectUx.DismissOffer();
		m_ReconnectUx.StopWatchingForHostReturn();
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
		if (m_State != NetMatchServiceState::Failed || m_IsHost || !m_MatchWasRunning || m_HostEndedTheMatch) {
			return false;
		}
		return m_TicketStore.HasRecord();
	}

	void NetMatchService::DriveReconnectUx(uint64_t nowMs) {
		NetMatchServiceState state = NetMatchServiceState::Idle;
		bool isHost = false;
		bool hasRecord = false;
		bool matchWasRunning = false;
		bool substituteCommitted = false;
		std::string reason;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			state = m_State;
			isHost = m_IsHost;
			substituteCommitted = m_ReconnectClient.GetState() == NetH4ClientState::Joined && m_ReconnectClient.GetStats().substitutionAcksSent > 0;
			hasRecord = m_TicketStore.HasRecord();
			matchWasRunning = m_MatchWasRunning;
			reason = m_ErrorText;
			// The host gave this seat away or released it: the answer is final, so the ticket goes and nothing retries.
			const bool seatGone = state == NetMatchServiceState::Failed && !isHost && m_Session && m_Session->HasReject() &&
			                      (m_Session->GetRejectReason() == NetRejectReason::SeatReassigned || m_Session->GetRejectReason() == NetRejectReason::SeatReleased);
			if (seatGone) {
				if (!m_ReconnectUx.IsRefused()) {
					if (m_TicketStore.HasRecord()) (void)m_TicketStore.Clear(nullptr);
					m_ReconnectUx.NoteRefused(m_Session->BuildPlayerRefusalText());
				}
				return;
			}
			// The held seat's own rejoin is trying the hosts the match named; the prompt takes over only once it gives up.
			if (m_HeldRejoinDriving) return;
			// A match the host ended by leaving it is over for this seat: it lands, and nothing reconnects.
			if (m_HostEndedTheMatch) return;
		}
		if (state == NetMatchServiceState::Running || state == NetMatchServiceState::ReadyToLaunch) {
			if (m_ReconnectUx.IsActive()) {
				m_ReconnectUx.NoteReconnected(nowMs);
			} else if (m_ReconnectUx.GetState() == NetReconnectUxState::Idle) {
				m_ReconnectUx.NoteConnected(nowMs);
			}
			return;
		}
		// The host accepts an application only for a seat of a running match: the accepted substitute holds that seat's
		// credential and joins the way a returner does, through the round's image, never through a lobby round.
		if (state == NetMatchServiceState::Starting && !isHost && !m_SubstituteRejoinStarted && substituteCommitted) {
			m_SubstituteRejoinStarted = true;
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_MatchWasRunning = true;
			}
			System::PrintDiagnosticLine("[net-match] substitute accepted: joining the running match through its image");
			std::string error;
			if (!BeginTicketRejoin(&error)) System::PrintDiagnosticLine("[net-match] substitute join failed: " + error);
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
		// A seat that committed no frame of the round has nothing to reclaim: losing the host lands it at once.
		if (!m_ReconnectUx.IsActive()) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_LandedWithoutFrame) return;
			if (m_FailedWithoutFrame) {
				m_LandedWithoutFrame = true;
				m_ErrorText = "The host left the match";
				m_StatusText = m_ErrorText;
				System::PrintDiagnosticLine("[net-match] resync failed: The host left the match before this seat committed a frame; nothing to reclaim, landing");
				return;
			}
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
		std::optional<NetMatchServiceRequest> successor;
		{
			// The match may be hosted by one of the successors it published: every other attempt asks the next of them.
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (!m_HeldRejoinRoutes.empty() && (m_ReconnectRouteTurn++ % 2) == 1) {
				successor = m_HeldRejoinRoutes.front();
				m_HeldRejoinRoutes.pop_front();
				m_HeldRejoinRoutes.push_back(*successor);
			}
		}
		if (successor) System::PrintDiagnosticLine("[net-match] reconnect: trying the successor at " + successor->address + ":" + std::to_string(successor->port));
		if (!(successor ? RejoinSuccessorRoute(*successor, &attemptError) : BeginTicketRejoin(&attemptError))) {
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
		// The host's script state is taken on this thread, the one that runs the scripts, when the round is asked to start.
		bool capture = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			capture = m_IsHost && m_RoundStartScriptsWanted && m_RoundStartScripts.empty() && m_State == NetMatchServiceState::Starting;
		}
		if (capture) {
			std::vector<uint8_t> scripts;
			std::string captureError;
			if (!LuaMan::CaptureRoundStartScripts(scripts, &captureError)) {
				SetState(NetMatchServiceState::Failed, "Match start failed", "the round's start scripts could not be captured: " + captureError);
				return;
			}
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_RoundStartScripts = scripts;
			m_RoundStartScriptsToStream = std::move(scripts);
		}
		m_StartRequested.store(true);
	}

	bool NetMatchService::ConsumeReadyToLaunch(std::string& outActivityPreset) {
		Update();
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_State != NetMatchServiceState::ReadyToLaunch || !m_Coordinator) {
			return false;
		}
		const auto& launchConfig = m_Coordinator->GetConfig();
		const bool freshRound = !launchConfig.continuesMatch && !launchConfig.resumeFromSnapshot && !launchConfig.joinsRunningRound &&
		    !m_Coordinator->IsPersistentWorldRound() && !m_WorldCatchUp.active;
		if (freshRound && m_PendingRoundStartScripts.empty()) {
			m_State = NetMatchServiceState::Failed;
			m_StatusText = "Match start failed";
			m_ErrorText = "The fresh round has no host script state";
			return false;
		}
		// Every peer, the host included, starts the round on the host's script state; a peer that cannot take it does not start.
		if (!m_PendingRoundStartScripts.empty()) {
			const std::vector<uint8_t> scripts = std::move(m_PendingRoundStartScripts);
			m_PendingRoundStartScripts.clear();
			std::string restoreError;
			if (!LuaMan::RestoreRoundStartScripts(scripts, &restoreError)) {
				m_State = NetMatchServiceState::Failed;
				m_StatusText = "Match start failed";
				m_ErrorText = "This game could not take the host's script state: " + restoreError;
				System::PrintDiagnosticLine("[net-match] round start scripts refused: " + restoreError);
				return false;
			}
			System::PrintDiagnosticLine(std::format("[net-match] round start scripts restored bytes={}", scripts.size()));
		}
		m_LaunchedFreshRound = freshRound;
		if (freshRound) g_MovableMan.RestartSimUpdateFrameNumber();
		const uint64_t launchRound = m_WorldCatchUp.active && m_WorldCatchUp.privateMatch ? m_WorldCatchUp.roundId : m_Coordinator->GetRoundId();
		ScenarioRunner::SetLockstepCoordinator(m_Coordinator.get(), m_PendingResyncState.has_value());
		m_Coordinator->DeferStopsToTickBoundary();
		m_Coordinator->SetSeatStateSource(&NetMatchService::QuerySeatState, this);
		// The lockstep wait parks the sim thread; without this the plane could not answer a leave or a
		// reclaim while the round waits on the very peer that sent it.
		ScenarioRunner::SetSessionPump([this] { PumpSessionEvents(); }, [this] {
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (!m_IsHost || !m_Coordinator) return false;
			// A seat we hold has not been told the round ended: its return is still owed the goodbye, and the
			// drain's idle budget is what bounds the wait for it.
			if (m_GoodbyeOwedToRejoiners) return true;
			const uint64_t completed = m_Coordinator->GetResumeFrame();
			for (const auto& session: m_WorldJoin.Sessions()) {
				if ((session.phase == NetWorldJoinPhase::SnapshotTransfer || session.phase == NetWorldJoinPhase::CatchingUp) &&
				    session.acknowledgedThrough + 1 < completed) return true;
			}
			return false;
		}, [this] { return RejoinProgressSum(); }, [this] {
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (!m_IsHost || !m_Session || !m_Coordinator) return;
			const uint64_t resume = m_Coordinator->GetResumeFrame();
			const uint64_t finalFrame = m_CompletedRoundFinalFrame != 0 ? m_CompletedRoundFinalFrame : (resume > 0 ? resume - 1 : 0);
			AnswerEndedReturnersLocked(finalFrame);
		});
		ScenarioRunner::SetHeldCatchUp([this] { return BeginInPlaceCatchUp(); });
		// The coordinator owns the transport queue during the match; reconnect handshakes hand over
		// here and drain through PumpSessionEvents on the same (game) thread.
		DiscardUndeliveredSessionEventsLocked();
		AttachCoordinatorSessionSink();
		if (m_Runner) {
			std::string recordError;
			// A round that stands on a checkpoint records a segment from its first frame: an ordinary
			// file names no world, so playing it back would boot the preset at a mid-world tick.
			// The host's own option arms the recorder for a menu-hosted match, through the path
			// -net-replay-out uses; a flag that already named a file keeps it.
			if (m_IsHost && !ScenarioRunner::IsLockstepReplayRecordArmed() && g_SettingsMan.GetNetworkRecordReplays()) {
				std::error_code directoryError;
				const std::filesystem::path replays = std::filesystem::path("Userdata") / "Replays";
				std::filesystem::create_directories(replays, directoryError);
				const std::string name = "match-" + std::to_string(m_Runner->GetMatchConfig().sessionId) + "-r" +
				                         std::to_string(m_Coordinator->GetRoundId()) + AutosaveStore::c_SegmentExtension;
				ScenarioRunner::ArmLockstepReplayRecordForRound((replays / name).string());
			}
			// A heal stands on the checkpoint the host named for it, so its round chains as well.
			uint64_t healTick = 0;
			std::string healDigest;
			if (m_ResumeSegmentTick == 0 && m_PendingResyncState && m_PendingResyncState->rewindTick != 0) {
				const std::string healMatchId = m_PendingResyncState->rewindMatchId.empty() ? m_AutosaveMatchId : m_PendingResyncState->rewindMatchId;
				AutosaveDescriptor healed;
				if (!healMatchId.empty() &&
				    AutosaveStore::Validate(AutosaveStore::ArchivePath(AutosaveStore::Directory(), healMatchId, m_PendingResyncState->rewindTick), healed)) {
					healTick = healed.savedTick;
					healDigest = healed.worldStructureHash;
				}
			}
			const RoundRecordingPlan plan = PlanRoundRecording(m_Runner->GetMatchConfig().persistentWorld, m_WorldIdentity.IsValid(),
			                                                   m_ResumeSegmentTick, m_ResumeArchiveDigest, healTick, healDigest);
			if (plan.segment) {
				NetWorldSegmentHeader header;
				header.worldId = m_WorldIdentity.worldId;
				header.tick = plan.tick;
				header.round = m_AutosaveIdentity.roundId;
				header.boot = m_WorldIdentity.boot;
				header.worldDigest = plan.digest;
				const std::string path = AutosaveStore::SegmentPath(AutosaveStore::Directory(), header.worldId, header.tick).string();
				if (!ScenarioRunner::BeginLockstepWorldSegmentRecord(m_Runner->GetMatchConfig(), header, path, &recordError) && !recordError.empty()) {
					{
						std::ostringstream line;
						line << "[net-world] resumed round records nothing: " << recordError;
						System::PrintDiagnosticLine(line.str());
					}
				}
			} else {
				(void)ScenarioRunner::BeginLockstepReplayRecord(m_Runner->GetMatchConfig(), &recordError);
			}
			m_ResumeSegmentTick = 0;
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
				// Both peers must be able to name the same checkpoint, so the id is the match, not the machine.
				m_AutosaveMatchId = m_Runner ? std::format("{:016x}-{:016x}", m_Runner->GetMatchConfig().sessionId, launchRound) : "";
			}
			m_MatchAutosaveSeconds = m_Runner ? MatchAutosaveSeconds(m_Runner->GetMatchConfig()) : 0;
			m_AutosaveIdentity.sessionId = m_Runner ? m_Runner->GetMatchConfig().sessionId : 0;
			m_AutosaveIdentity.roundId = launchRound;
			m_AutosaveIdentity.intervalSeconds = m_MatchAutosaveSeconds;
			m_AutosaveIdentity.pinnedCheckpointSource = m_PinnedAutosave;
			// A new match pins nothing: the previous round's rewind point must not hold an archive here. A seat rejoining its
			// own round is not a new match: it keeps the checkpoint that round agreed to rewind to.
			const bool privateReturn = m_WorldCatchUp.active && m_WorldCatchUp.privateMatch;
			if (privateReturn && m_RewindAnchorTick != 0 && m_RewindAnchorMatchId == m_AutosaveMatchId) {
				const std::optional<AutosaveDescriptor> anchor = AutosaveStore::Find(m_RewindAnchorMatchId, m_RewindAnchorTick, nullptr);
				m_PinnedAutosave->Store(anchor ? anchor->roundId : launchRound, m_RewindAnchorTick);
			} else {
				m_PinnedAutosave->Store(0, 0);
			}
			m_NextAutosaveSimTime = -1;
			m_LastAutosaveSimTime = -1;
			ResetCheckpointSchedule();
		} else {
			ForgetOpenCaptureOnHeal();
			// A member streamed the world's checkpoint keeps the world's chain from here, as one that held it does.
			if (m_AutosaveMatchId.empty() && m_Runner && m_Runner->GetMatchConfig().persistentWorld && !m_Runner->GetMatchConfig().worldId.empty()) {
				m_AutosaveMatchId = m_Runner->GetMatchConfig().worldId;
				m_AutosaveIdentity.sessionId = m_Runner->GetMatchConfig().sessionId;
				m_AutosaveIdentity.roundId = launchRound;
				m_AutosaveIdentity.intervalSeconds = MatchAutosaveSeconds(m_Runner->GetMatchConfig());
				m_AutosaveIdentity.pinnedCheckpointSource = m_PinnedAutosave;
			}
		}
		// Every round writes its checkpoints under the configuration it is actually played on, so a
		// resumed match's own checkpoints can be resumed again.
		SeatRestartConfigLocked(m_Runner ? m_Runner->GetMatchConfig() : m_MatchConfig);
		if (!m_PendingResyncState.has_value() || m_CurrentMatchSummary.peers.empty()) {
			m_LastMatchSummary.reset();
			m_CurrentMatchSummary = {};
			m_SummarySeats = m_SeatViews;
			const auto& config = m_Runner ? m_Runner->GetMatchConfig() : m_Coordinator->GetConfig().matchConfig;
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
		ResetRoundGoodbyeLocked();
		m_HeldRejoinDriving = false;
		m_HeldRejoinRoutes.clear();
		m_State = NetMatchServiceState::Running;
		m_StatusText = "Match running";
		m_MatchAutosaveSeconds = m_Runner ? MatchAutosaveSeconds(m_Runner->GetMatchConfig()) : 0;
		if (m_IsHost)
			m_ReconnectHost.SetMigrationHold(false, AdmissionNowMs());
		// A staged options draft is a lobby-round intent: the launch that ran without it retiring
		// means its window closed, so it must not surface again in the rematch lobby.
		m_PendingHostOptions.reset();
		m_HostOptionsRequest.Clear();
		const NetLockstepConfig& config = m_Coordinator->GetConfig();
		if (m_WorldCatchUp.active) {
			{
				std::ostringstream line;
				line << "[net-match] bootstrap checkpoint=" << m_WorldCatchUp.snapshotTick << " local_peer=" << static_cast<int>(m_LocalPeerId);
				System::PrintDiagnosticLine(line.str());
			}
		} else {
			System::PrintDiagnosticLine(std::format("[net-lockstep] start round={} frame={} local_peer={} peers={} input_delay={}\n",
			                         m_Coordinator->GetRoundId(), config.startFrame, config.localPeerId, config.peerCount, config.inputDelayFrames));
		}
		CaptureA7SeatView();
		return true;
	}

	namespace {
		std::string ResumeHex(const std::vector<uint8_t>& bytes) {
			static constexpr char Digits[] = "0123456789abcdef";
			std::string hex;
			hex.reserve(bytes.size() * 2);
			for (uint8_t byte: bytes) {
				hex.push_back(Digits[byte >> 4]);
				hex.push_back(Digits[byte & 0x0F]);
			}
			return hex;
		}

		bool ResumeBytes(const std::string& hex, std::vector<uint8_t>& out) {
			if (hex.empty() || hex.size() % 2 != 0) return false;
			out.clear();
			out.reserve(hex.size() / 2);
			for (size_t index = 0; index < hex.size(); index += 2) {
				uint8_t value = 0;
				for (size_t half = 0; half < 2; ++half) {
					const char digit = hex[index + half];
					const int nibble = digit >= '0' && digit <= '9' ? digit - '0' : (digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 : -1);
					if (nibble < 0) return false;
					value = static_cast<uint8_t>((value << 4) | static_cast<uint8_t>(nibble));
				}
				out.push_back(value);
			}
			return true;
		}

		/// The lobby payload bytes that carried a configuration, kept so a restart republishes the very
		/// configuration the peers hashed instead of rebuilding one that only looks like it. The copy is written
		/// down (a restart manifest, an image's offer and its log line), so it holds no relay login.
		bool EncodeConfigPayload(const NetMatchConfig& config, std::string& outHex) {
			std::vector<uint8_t> bytes;
			if (!NetLobbyProtocol::Encode(NetLobbyMessage{NetLobbyMatchConfig{NetMatchConfigUtil::WithoutRelay(config)}}, bytes)) return false;
			outHex = ResumeHex(bytes);
			return true;
		}

		bool DecodeConfigPayload(const std::string& hex, NetMatchConfig& out) {
			std::vector<uint8_t> bytes;
			if (!ResumeBytes(hex, bytes)) return false;
			const auto decoded = NetLobbyProtocol::Decode(bytes);
			const auto* message = decoded.ok ? std::get_if<NetLobbyMatchConfig>(&decoded.message.payload) : nullptr;
			if (!message) return false;
			out = message->config;
			return true;
		}
	}

	void NetMatchService::SeatRestartConfigLocked(const NetMatchConfig& config) {
		m_AutosaveIdentity.worldBoot = config.persistentWorld ? config.worldBoot : 0;
		// The exact bytes the peers hashed, so a restart republishes that configuration rather than one
		// rebuilt from today's settings.
		std::string payload;
		if (!EncodeConfigPayload(config, payload)) {
			m_AutosaveIdentity.configPayload.clear();
			m_AutosaveIdentity.configHash.clear();
			m_AutosaveIdentity.peerNames.clear();
			return;
		}
		m_AutosaveIdentity.configPayload = std::move(payload);
		m_AutosaveIdentity.configHash = NetMatchConfigUtil::StoredConfigHash(config);
		m_AutosaveIdentity.peerNames.clear();
		for (const NetMatchPlayerSlot& slot: config.players) {
			if (!slot.cpu && !slot.displayName.empty()) m_AutosaveIdentity.peerNames.push_back(slot.displayName);
		}
	}

	bool NetMatchService::PrivateBaseRefreshDue(bool seatHeld, uint64_t staleFrom, uint64_t baseTick, double steadyCaptureMs) {
		// A capture stalls every peer, so no seat asks for another base while the steady capture cost is past the bound;
		// the round's first capture pays a one-time warm-up and never decides alone.
		const bool captureWithinBudget = steadyCaptureMs < 0.0 || steadyCaptureMs <= 50.0;
		return captureWithinBudget && (seatHeld || staleFrom > baseTick);
	}

	double NetMatchService::SteadyCaptureMs(const std::deque<double>& costs) {
		if (costs.empty()) return -1.0;
		std::vector<double> sorted(costs.begin(), costs.end());
		std::sort(sorted.begin(), sorted.end());
		return sorted.size() % 2 ? sorted[sorted.size() / 2] : (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]) / 2.0;
	}

	bool NetMatchService::PrivateBaseWantedLocked(uint64_t nowMs) const {
		const bool returnerWaiting = std::any_of(m_WorldJoin.Sessions().begin(), m_WorldJoin.Sessions().end(), [](const NetWorldJoinSession& session) {
			return session.phase == NetWorldJoinPhase::SnapshotTransfer && !session.transferStarted;
		});
		if (!returnerWaiting) return false;
		if (m_PrivateImageRecapture || !m_WorldJoin.Image().IsValid()) return true;
		// Returners that arrive together share one base; an older one is retaken unless capturing costs past the bound.
		const bool fresh = m_PrivateImageTakenMs != 0 && nowMs - m_PrivateImageTakenMs < c_PrivateImageMinIntervalMs;
		return !fresh && PrivateBaseRefreshDue(true, 0, m_WorldJoin.Image().tick, SteadyCaptureMs(m_PrivateCaptureCosts));
	}

	bool NetMatchService::ReadPrivateBaseLocked(uint64_t tick, NetWorldCheckpointImage& image, std::string* error) {
		const auto& config = m_Coordinator->GetConfig();
		const uint64_t round = m_Coordinator->GetRoundId();
		NetResyncState state;
		if (!ScenarioRunner::CaptureNetResyncState(tick, state, error)) return false;
		state.pendingInputs.clear(); state.pendingCommands.clear(); state.pendingPlayerBindings.clear(); state.admittedReseats.clear();
		image.privateSessionId = config.sessionId; image.round = round; image.tick = tick;
		image.pauseState = ScenarioRunner::CaptureLockstepPauseState();
		image.authorityGeneration = config.migrationGeneration;
		image.authorityPeerId = m_Coordinator->GetHostPeerId();
		for (const auto& [peer, frame]: m_Coordinator->GetPeerLeaveFrames()) if (frame <= tick) image.departedPeers[peer] = frame;
		image.roundConfigHash = NetIdentity::HashHex(m_Coordinator->GetRoundConfigHash());
		image.configRevision = config.matchConfig.configRevision;
		image.matchConfigHash = NetIdentity::HashHex(NetMatchConfigUtil::HashConfig(config.matchConfig));
		std::vector<uint8_t> side;
		if (!EncodeConfigPayload(config.matchConfig, image.checkpointConfig) || !NetResyncCodec::Encode(state, {0}, side, error)) return false;
		image.sideState = ResumeHex(side);
		NetLockstepFrame holds;
		holds.targetFrame = tick; holds.roundId = round;
		for (const auto& [peer, hold]: m_Coordinator->HeldTransactions()) holds.commands.push_back({m_Coordinator->GetHostPeerId(), hold});
		if (!EncodeCommittedJoinFrame(holds, side, error)) return false;
		image.heldState = ResumeHex(side);
		return true;
	}

	void NetMatchService::PreparePrivateRejoinCheckpoint() {
		const char* held = !m_IsHost ? nullptr : m_State != NetMatchServiceState::Running ? "the service is not running" : !m_Coordinator ? "there is no round" :
		                   !m_Coordinator->IsRunning() ? "the round is not running" : !m_Coordinator->UsesBoundedWait() ? "the round has no bounded wait" :
		                   m_Coordinator->IsPersistentWorldRound() ? "the round is a world" : m_Coordinator->IsMigrating() ? "the round is migrating" :
		                   !g_ActivityMan.ActivityRunning() ? "the activity is not running" : nullptr;
		if (held || !m_IsHost) {
			// A returner that waits on a base the host cannot take says why, once per reason.
			if (held && held != m_PrivateBaseHeldReason && std::any_of(m_WorldJoin.Sessions().begin(), m_WorldJoin.Sessions().end(), [](const NetWorldJoinSession& session) { return session.phase == NetWorldJoinPhase::SnapshotTransfer; })) {
				System::PrintDiagnosticLine(std::string("[net-match] private base waits: ") + held);
				m_PrivateBaseHeldReason = held;
			}
			return;
		}
		m_PrivateBaseHeldReason = nullptr;
		const uint64_t round = m_Coordinator->GetRoundId();
		if (round == 0) return;
		const auto& config = m_Coordinator->GetConfig();
		std::string error;
		const std::string name = "p5join_base_" + std::to_string(System::GetProcessID()) + "_" + std::to_string(round);
		if (m_PrivateImageRound != round) {
			// The rejoin plane opens with the round and its committed tail is kept from here; no base is captured yet.
			m_PrivateImageRound = round;
			m_PrivateImageTakenMs = 0;
			m_PrivateImageRecapture = false;
			m_PrivateCaptureCosts.clear();
			m_PrivateActivations.clear();
			m_PrivateJoinBlobs.clear();
			m_PrivateJoinError.clear();
			m_InPlaceIncarnationBumps.clear();
			auto admissionConfig = config.matchConfig; admissionConfig.hostPeerId = m_Coordinator->GetHostPeerId();
			if (!m_WorldJoin.ConfigureMatchRejoins(admissionConfig, round, config.simTickMs, &error)) {
				// A plane of an earlier round never serves this one's returners.
				m_WorldJoin.Reset();
				m_PrivateJoinError = error.empty() ? "the rejoin plane did not open" : error;
				System::PrintDiagnosticLine("[net-match] the rejoin plane of round " + std::to_string(round) + " did not open: " + m_PrivateJoinError);
				return;
			}
			// A successor serves the seats its predecessor held from its own record of the round, so a held seat keeps its world.
			if (m_CommittedRing.Count() > 0 && m_CommittedRing.LastFrame() + 1 >= static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()) && m_WorldJoin.Tail().AdoptRecords(m_CommittedRing)) {
				System::PrintDiagnosticLine("[net-match] the rejoin plane opens on this peer's own record of the round: frames " + std::to_string(m_CommittedRing.FirstFrame()) +
				                            ".." + std::to_string(m_CommittedRing.LastFrame()) + " round=" + std::to_string(round));
			}
			m_CommittedRing.Clear();
			// A returning seat's base is this host's own archive of an announced capture; the writer thread hashes what it wrote.
			g_ActivityMan.SetAutosaveDigest([](const std::vector<uint8_t>& bytes) { return DigestWorldJoinBytes(bytes); });
			m_WorldJoin.Tail().EnableJournal(g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/" + name + ".ccsave.inputs");
			return;
		}
		// A held seat costs the survivors nothing while it is away: its base is captured when its player is back, never on a cadence.
		const uint64_t nowMs = SteadyNowMs();
		const bool transferring = std::any_of(m_WorldJoin.Sessions().begin(), m_WorldJoin.Sessions().end(), [](const NetWorldJoinSession& session) {
			return session.phase == NetWorldJoinPhase::SnapshotTransfer && session.transferStarted;
		});
		// A returning seat's base is an announced capture: the checkpoint schedule names its tick on the committed stream, every
		// peer collects every Lua state at that tick's end and takes the capture with no save hook, so the image holds exactly
		// the state every live peer holds, its garbage included.
		const bool announcedBase = !m_AutosaveMatchId.empty();
		if (announcedBase && m_PrivateBaseTick != 0) {
			const uint64_t baseTick = std::exchange(m_PrivateBaseTick, 0);
			if (baseTick == static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount())) {
				NetWorldCheckpointImage image;
				if (!ReadPrivateBaseLocked(baseTick, image, &error)) { m_PrivateJoinError = error; m_PrivateBaseRequested = true; m_WorldCapturePending = true; return; }
				m_PrivateBasePending = std::move(image);
				System::PrintDiagnosticLine("[net-match] private base taken at the announced tick " + std::to_string(baseTick) + "; its archive follows from the writer");
				return;
			}
		}
		if (!PrivateBaseWantedLocked(nowMs) || m_PrivateImageTask.valid() || transferring || m_PrivateBasePending || m_PrivateBaseRequested ||
		    m_Coordinator->HasSeatReclaimGap(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()))) return;
		if (announcedBase) {
			m_PrivateCaptureCold = m_PrivateImageTakenMs == 0;
			m_PrivateImageTakenMs = nowMs;
			m_PrivateImageRecapture = false;
			m_PrivateJoinError.clear();
			m_PrivateBaseRequested = true;
			m_WorldCapturePending = true;
			System::PrintDiagnosticLine("[net-match] private base asked of the checkpoint schedule at tick " + std::to_string(g_TimerMan.GetSimUpdateCount()));
			return;
		}
		const bool ownsKeepalive = !m_SnapshotLoadKeepalive.joinable();
		StartSnapshotLoadKeepalive();
		struct FinishCapture {
			NetMatchService& service;
			bool owns;
			~FinishCapture() { if (owns) service.StopSnapshotLoadKeepalive(); }
		} finishCapture{*this, ownsKeepalive};
		m_PrivateCaptureCold = m_PrivateImageTakenMs == 0;
		m_PrivateImageTakenMs = nowMs;
		m_PrivateImageRecapture = false;
		m_PrivateJoinError.clear();
		const uint64_t tick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
		NetResyncState state;
		if (!ScenarioRunner::CaptureNetResyncState(tick, state, &error)) { m_PrivateJoinError = error; return; }
		state.pendingInputs.clear(); state.pendingCommands.clear(); state.pendingPlayerBindings.clear(); state.admittedReseats.clear();
		NetWorldCheckpointImage image;
		image.privateSessionId = config.sessionId; image.round = round; image.tick = tick;
		image.pauseState = ScenarioRunner::CaptureLockstepPauseState();
		image.authorityGeneration = config.migrationGeneration;
		image.authorityPeerId = m_Coordinator->GetHostPeerId();
		for (const auto& [peer, frame]: m_Coordinator->GetPeerLeaveFrames()) if (frame <= tick) image.departedPeers[peer] = frame;
		image.roundConfigHash = NetIdentity::HashHex(m_Coordinator->GetRoundConfigHash());
		image.configRevision = config.matchConfig.configRevision;
		image.matchConfigHash = NetIdentity::HashHex(NetMatchConfigUtil::HashConfig(config.matchConfig));
		std::vector<uint8_t> side;
		if (!EncodeConfigPayload(config.matchConfig, image.checkpointConfig) || !NetResyncCodec::Encode(state, {0}, side, &error)) { m_PrivateJoinError = error; return; }
		image.sideState = ResumeHex(side);
		NetLockstepFrame holds;
		holds.targetFrame = tick; holds.roundId = round;
		for (const auto& [peer, hold]: m_Coordinator->HeldTransactions()) holds.commands.push_back({m_Coordinator->GetHostPeerId(), hold});
		if (!EncodeCommittedJoinFrame(holds, side, &error)) { m_PrivateJoinError = error; return; }
		image.heldState = ResumeHex(side);
		image.path = g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/" + name + ".ccsave";
		const auto began = std::chrono::steady_clock::now();
		if (!g_ActivityMan.SaveCurrentGame(name)) { m_PrivateJoinError = "the private checkpoint could not be captured"; return; }
		image.captureMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
		const auto saved = g_ActivityMan.GetSaveGameTask();
		m_PrivateImageTask = std::async(std::launch::async, [saved, image = std::move(image)]() mutable {
			PrivateJoinImage result;
			result.image = std::move(image);
			try {
				if (!saved.get()) { result.error = "the initial private checkpoint writer failed"; return result; }
				std::ifstream input(result.image.path, std::ios::binary);
				auto archive = std::make_shared<std::vector<uint8_t>>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
				if (archive->empty()) { result.error = "the initial private checkpoint is empty"; return result; }
				result.image.bytes = archive->size(); result.image.digest = DigestWorldJoinBytes(*archive);
				result.archive = std::move(archive);
				input.close();
				std::error_code ignored;
				std::filesystem::remove(result.image.path, ignored);
			} catch (const std::exception& exception) { result.error = exception.what(); }
			return result;
		});
	}

	bool NetMatchService::ReadCommittedJoinFrame(const NetLockstepCoordinator& coordinator, uint64_t tick, NetLockstepReadyFrame& ready) {
		if (coordinator.PeekReadyFrame(tick, ready)) return true;
		if (tick < coordinator.GetConfig().startFrame || tick >= coordinator.GetStats().effectiveStartFrame) return false;
		ready = {}; ready.frame = tick;
		return true;
	}

	// The capture's verdict comes from the writer thread a tick or more after the simulation queued it.
	// A world capture's bookkeeping follows that verdict, so nothing stands on an archive that never lands.
	void NetMatchService::ApplyAutosaveVerdict(uint64_t tick, bool joinCapture, bool archived) {
		if (archived) m_LastMatchSaveTime = static_cast<int64_t>(std::time(nullptr));
		if (!archived && m_IsHost && tick == m_ManualSaveTakenTick) m_ManualSaveFailure = "this machine's capture was refused";
		if (!archived) {
			if (joinCapture) m_WorldCapturePending = true;
			// A refused base is asked again; the metadata read at its tick stands for nothing.
			if (joinCapture && m_PrivateBasePending && m_PrivateBasePending->tick == tick) { m_PrivateBasePending.reset(); m_PrivateBaseRequested = true; }
			ScenarioRunner::DropPendingLockstepWorldSegment("the checkpoint at tick " + std::to_string(tick) + " was refused");
			return;
		}
		if (joinCapture) {
			m_WorldCaptureRequestedTick = tick;
			m_WorldCapturePending = false;
		}
		// Every checkpoint wants a current admission file beside it; the pump writes it.
		m_RestartAdmissionDue.store(true);
	}

	void NetMatchService::NoteWorldJoinWantsCapture() {
		// The writer's verdict can land after its image was published: an image at or past the asked tick has answered that capture.
		if (m_WorldCaptureRequestedTick != 0 && m_WorldJoin.Image().tick >= m_WorldCaptureRequestedTick) m_WorldCaptureRequestedTick = 0;
		if (m_WorldCaptureRequestedTick == 0) m_WorldCapturePending = true;
	}

	void NetMatchService::ResolveAwaitedAutosave(uint64_t tick, bool archived) {
		const auto awaited = std::find_if(m_AwaitedAutosaves.begin(), m_AwaitedAutosaves.end(),
		                                  [&](const AwaitedAutosave& entry) { return entry.tick == tick; });
		if (awaited == m_AwaitedAutosaves.end()) return;
		const bool joinCapture = awaited->joinCapture;
		m_AwaitedAutosaves.erase(awaited);
		ApplyAutosaveVerdict(tick, joinCapture, archived);
	}

	std::vector<uint64_t> NetMatchService::TakeAutosaveVerdicts(std::vector<uint64_t>* refused) {
		std::vector<uint64_t> finished;
		while (const std::optional<ActivityMan::AutosaveVerdict> verdict = g_ActivityMan.TakeAutosaveVerdict()) {
			const bool awaited = std::any_of(m_AwaitedAutosaves.begin(), m_AwaitedAutosaves.end(), [&](const AwaitedAutosave& entry) { return entry.tick == verdict->tick; });
			ResolveAwaitedAutosave(verdict->tick, verdict->archived);
			if (awaited) finished.push_back(verdict->tick);
			if (awaited && !verdict->archived && refused) refused->push_back(verdict->tick);
		}
		return finished;
	}

	void NetMatchService::ResetCheckpointSchedule() {
		m_ScheduledCaptures.clear();
		m_OpenCaptureTick = 0;
		m_CaptureWriters.clear();
		m_CheckpointHolders.clear();
		m_OpenCaptureForJoin = false;
		m_ManualCaptures.clear();
		m_OpenCaptureManual = false;
		m_ManualSaveAsked = false;
		m_ManualSaveWaitShown = false;
		m_ManualSaveTakenTick = 0;
		m_ManualSaveFailure.clear();
		m_ManualSaveBusy = false;
		m_ManualSaveFailed = false;
		m_LastMatchSaveTime = 0;
		(void)ScenarioRunner::TakeAppliedCheckpoints();
	}

	void NetMatchService::ForgetOpenCaptureOnHeal() {
		// A heal restarts the stream the writers' reports rode, so the host names afresh instead of waiting on reports it dropped.
		m_OpenCaptureTick = 0;
		m_CaptureWriters.clear();
		m_OpenCaptureForJoin = false;
		// The host's ask the heal cut short is taken again after it.
		if (m_OpenCaptureManual) m_ManualSaveAsked = true;
		m_OpenCaptureManual = false;
	}

	void NetMatchService::NoteCheckpointHolder(uint64_t tick, uint8_t peer) {
		m_CheckpointHolders[tick].insert(peer);
		// Retention keeps a handful of archives, so only the newest captures' holders can matter.
		while (m_CheckpointHolders.size() > 32) m_CheckpointHolders.erase(m_CheckpointHolders.begin());
	}

	std::optional<AutosaveDescriptor> NetMatchService::ChooseRewindAnchor(const std::vector<AutosaveDescriptor>& validatedNewestFirst,
	                                                                     const std::map<uint64_t, std::set<uint8_t>>& holders, uint64_t savedTick,
	                                                                     const std::set<uint8_t>& peers, uint64_t roundId) {
		for (const AutosaveDescriptor& checkpoint: validatedNewestFirst) {
			if (checkpoint.savedTick > savedTick || checkpoint.roundId != roundId) continue;
			const auto held = holders.find(checkpoint.savedTick);
			if (held != holders.end() && std::includes(held->second.begin(), held->second.end(), peers.begin(), peers.end())) return checkpoint;
		}
		return std::nullopt;
	}

	std::set<uint8_t> NetMatchService::CheckpointWriters(uint64_t tick) const {
		std::set<uint8_t> writers;
		if (!m_Coordinator) return writers;
		writers.insert(GetLocalPeerId());
		// A held or departed seat is not simulating the tick live, so it neither captures nor reports.
		for (const auto& [peer, transport]: m_Coordinator->RemoteTransports()) {
			if (!m_Coordinator->HasHeldAISeat(peer) && !m_Coordinator->IsPeerGoneAtFrame(peer, tick)) writers.insert(peer);
		}
		return writers;
	}

	NetMatchService::AutosaveTickOutput NetMatchService::StepAutosaveSchedule(const AutosaveTickInput& input) {
		AutosaveTickOutput output;
		for (const CheckpointNote& note: input.applied) {
			if (note.kind == NetGameCheckpoint::Capture || note.kind == NetGameCheckpoint::ManualCapture) {
				// A capture named for a tick already behind this frame is taken here, on every peer alike.
				const uint64_t takenAt = std::max(note.tick, input.tick);
				m_ScheduledCaptures.insert(takenAt);
				if (note.kind == NetGameCheckpoint::ManualCapture) m_ManualCaptures.insert(takenAt);
				if (m_Coordinator) m_Coordinator->NoteAnnouncedCapture(takenAt);
				// The writers report the tick they took, so that is the capture the host waits on.
				if (m_IsHost && note.tick == m_OpenCaptureTick) { m_OpenCaptureTick = takenAt; m_OpenCaptureApplied = true; }
			} else if ((note.kind == NetGameCheckpoint::Written || note.kind == NetGameCheckpoint::Missed) && m_IsHost && note.tick == m_OpenCaptureTick) {
				m_CaptureWriters.erase(note.sender);
				if (m_OpenCaptureManual && note.kind == NetGameCheckpoint::Written) m_ManualSaveReported.insert(note.sender);
			}
			// Every peer keeps who holds what, so a host that takes the round over names the same rewind points.
			if (note.kind == NetGameCheckpoint::Written) NoteCheckpointHolder(note.tick, note.sender);
		}
		// A finished capture frees this peer's writer, and the host schedules on nothing else; a refused one says it holds nothing.
		for (const uint64_t finished: input.finished) {
			const bool refused = std::find(input.refused.begin(), input.refused.end(), finished) != input.refused.end();
			output.send.push_back({0, refused ? NetGameCheckpoint::Missed : NetGameCheckpoint::Written, finished});
			if (!refused && input.localPeer != 0) NoteCheckpointHolder(finished, input.localPeer);
			// The host's own report rides no frame while its seat is held, so it never waits on the stream for what it knows itself.
			if (m_IsHost && input.localPeer != 0 && finished == m_OpenCaptureTick) {
				m_CaptureWriters.erase(input.localPeer);
				if (m_OpenCaptureManual && !refused) m_ManualSaveReported.insert(input.localPeer);
			}
		}
		if (!m_ScheduledCaptures.empty() && *m_ScheduledCaptures.begin() <= input.tick) {
			m_ScheduledCaptures.erase(m_ScheduledCaptures.begin(), m_ScheduledCaptures.upper_bound(input.tick));
			output.capture = true;
		}
		// A provisional host's play may never be the match's: it takes no capture, and reports the one it was named as missed.
		if (output.capture && input.hostProvisional) {
			output.capture = false;
			m_ManualCaptures.erase(m_ManualCaptures.begin(), m_ManualCaptures.upper_bound(input.tick));
			output.send.push_back({0, NetGameCheckpoint::Missed, input.tick});
			if (m_IsHost && input.localPeer != 0) m_CaptureWriters.erase(input.localPeer);
		}
		if (output.capture && !m_ManualCaptures.empty() && *m_ManualCaptures.begin() <= input.tick) {
			m_ManualCaptures.erase(m_ManualCaptures.begin(), m_ManualCaptures.upper_bound(input.tick));
			output.manual = true;
		}
		if (!m_IsHost) return output;
		bool ask = input.manualRequested;
		if (ask && input.hostProvisional) {
			output.manualRefused = true;
			ask = false;
		}
		// A capture whose tick passed without reaching the stream rode a frame the round never played (the host's own, while its seat
		// was held): no writer takes it, so the schedule names the next one instead of waiting on it for the rest of the round.
		if (m_OpenCaptureTick != 0 && !m_OpenCaptureApplied && input.tick > m_OpenCaptureTick) {
			System::PrintDiagnosticLine(std::format("[autosave] named tick={} never reached the stream; naming the next", m_OpenCaptureTick));
			m_OpenCaptureTick = 0;
			m_CaptureWriters.clear();
			// The host's ask was never taken, so it is asked again.
			ask = ask || m_OpenCaptureManual;
			m_OpenCaptureManual = false;
		}
		for (auto writer = m_CaptureWriters.begin(); writer != m_CaptureWriters.end();) {
			writer = input.writers.contains(*writer) ? std::next(writer) : m_CaptureWriters.erase(writer);
		}
		// A second ask before the capture it would name is taken is that same capture.
		if (ask && m_OpenCaptureManual && m_OpenCaptureTick != 0 && input.tick <= m_OpenCaptureTick) ask = false;
		if (m_OpenCaptureManual && m_OpenCaptureTick != 0 && input.tick > m_OpenCaptureTick && m_CaptureWriters.empty()) {
			output.manualSaved = true;
			output.manualReported = m_ManualSaveReported.size();
			output.manualWriters = m_ManualSaveWriters;
			m_OpenCaptureManual = false;
		}
		output.manualPending = ask;
		if (m_OpenCaptureTick != 0 && (input.tick <= m_OpenCaptureTick || !m_CaptureWriters.empty())) return output;
		m_OpenCaptureTick = 0;
		m_OpenCaptureForJoin = false;
		// Every peer keeps the schedule the host announced in the agreed config, not its own setting.
		const uint32_t seconds = m_MatchAutosaveSeconds;
		// A join asks for one capture and waits for its verdict; only a refused one asks again.
		const bool joinCapture = m_WorldCapturePending && m_WorldJoin.IsConfigured() &&
		                         std::none_of(m_AwaitedAutosaves.begin(), m_AwaitedAutosaves.end(), [](const AwaitedAutosave& entry) { return entry.joinCapture; });
		if (!joinCapture && !ask && seconds == 0) return output;
		// A park commits empty frames, so an activation inside one would never be stamped: nothing is named until it lands.
		// The startup frames before a round's agreed first frame carry no commands either, so a capture named in them never
		// reaches a writer and the schedule would wait on it for the rest of the round.
		if (input.activationPending || input.startupPending || input.ownSeatHeld || input.hostProvisional) return output;
		const int64_t tickLength = g_TimerMan.GetDeltaTimeTicks();
		const int64_t interval = static_cast<int64_t>(seconds) * g_TimerMan.GetTicksPerSecond();
		if (m_NextAutosaveSimTime < 0 || input.now < m_LastAutosaveSimTime) {
			m_NextAutosaveSimTime = input.now - tickLength + interval;
		}
		m_LastAutosaveSimTime = input.now;
		// The capture is named `lead` ticks ahead, so it reaches every peer's stream before its tick.
		const int64_t takenAt = input.now + static_cast<int64_t>(input.lead) * tickLength;
		// The host's ask never moves the interval: it is advanced only when its own capture is due.
		if (!joinCapture && !ask && takenAt < m_NextAutosaveSimTime) return output;
		if (interval > 0 && takenAt >= m_NextAutosaveSimTime) m_NextAutosaveSimTime += ((takenAt - m_NextAutosaveSimTime) / interval + 1) * interval;
		m_OpenCaptureTick = input.tick + input.lead;
		m_OpenCaptureApplied = false;
		m_OpenCaptureForJoin = joinCapture;
		m_CaptureWriters = input.writers;
		m_OpenCaptureManual = ask;
		if (ask) {
			m_ManualSaveReported.clear();
			m_ManualSaveWriters = input.writers.size();
			output.manualPending = false;
		}
		output.send.push_back({0, ask ? NetGameCheckpoint::ManualCapture : NetGameCheckpoint::Capture, m_OpenCaptureTick});
		return output;
	}

	void NetMatchService::AppendCommittedJoinFrame(uint64_t tick) {
		if (!m_Coordinator) return;
		// A peer with no tail of its own keeps the round's committed frames: as a successor it serves a held seat's catch-up from them.
		// The record is the round's from its first bounded tick to its end, through a handover that suspends the bounded wait.
		const uint64_t round = m_Coordinator->GetRoundId();
		if (!m_WorldJoin.IsPrivateMatch() && round != 0 && m_CommittedRingRound != round && !m_Coordinator->IsPersistentWorldRound() && m_Coordinator->UsesBoundedWait()) {
			const NetLockstepConfig& config = m_Coordinator->GetConfig();
			uint32_t margin = 0;
			for (uint8_t peer = 1; peer <= config.peerCount; ++peer) margin = std::max<uint32_t>(margin, NetMatchConfigUtil::PeerInputDelay(config.matchConfig, peer));
			// A handover renames the round, not its history: the successor keeps the record it made under the lost host, whichever of
			// this append and the rejoin plane's opening comes first after the boundary.
			const bool handover = m_CommittedRingRound != 0 && config.migrationGeneration > m_CommittedRingGeneration && m_CommittedRing.Count() > 0;
			if (handover) System::PrintDiagnosticLine("[net-match] the committed record goes on through the handover: frames " + std::to_string(m_CommittedRing.FirstFrame()) + ".." +
			                                          std::to_string(m_CommittedRing.LastFrame()) + " round=" + std::to_string(round));
			else m_CommittedRing.Clear();
			m_CommittedRing.Configure(NetWorldFrameLog::RingFrames(config.matchConfig.slowPlayerBoundTicks, margin, c_PrivateImageMinIntervalMs, g_TimerMan.GetDeltaTimeMS()), 0);
			m_CommittedRingRound = round;
			m_CommittedRingGeneration = config.migrationGeneration;
		}
		const bool ring = !m_WorldJoin.IsPrivateMatch() && round != 0 && m_CommittedRingRound == round;
		if (!m_WorldJoin.IsConfigured() && !ring) return;
		const auto record = [&](const NetLockstepFrame& frame) {
			// A new round, or a tick this peer did not commit, starts the record again: a successor never serves across a gap.
			if (ring && !m_CommittedRing.Append(frame, nullptr)) {
				m_CommittedRing.Clear();
				(void)m_CommittedRing.Append(frame, nullptr);
			}
		};
		// A tick a held seat's catch-up replayed is the committed frame its tail carried.
		if (!m_WorldJoin.IsConfigured() && ScenarioRunner::WorldCatchUpActive()) {
			NetLockstepFrame replayed;
			if (ScenarioRunner::TakeWorldCatchUpAppliedFrame(tick, replayed)) { replayed.roundId = round; record(replayed); }
			return;
		}
		NetLockstepReadyFrame ready;
		if (ReadCommittedJoinFrame(*m_Coordinator, tick, ready)) {
			auto frame = PackWorldJoinReadyFrame(ready); frame.roundId = round;
			std::string error;
			record(frame);
			if (m_WorldJoin.IsConfigured() && !m_WorldJoin.Tail().Append(frame, &error) && m_WorldJoin.IsPrivateMatch()) m_PrivateJoinError = "committed catch-up history: " + error;
			// A world's host keeps the round's history past its memory on disk too, so a slow joiner's tail is never evicted under it.
			if (m_IsHost && m_WorldJoin.IsConfigured() && !m_WorldJoin.IsPrivateMatch() && !m_WorldJoin.Tail().HasJournal() && m_WorldJoin.Tail().Count() != 0) {
				m_WorldJoin.Tail().EnableJournal(g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/world_history_" + std::to_string(System::GetProcessID()) + "_" + std::to_string(round) + ".ccsave.inputs");
			}
		} else if (m_WorldJoin.IsPrivateMatch()) m_PrivateJoinError = "the completed tick has no committed catch-up input";
		if (tick % 60 == 0) PruneReturnHistory(tick);
	}

	std::optional<uint64_t> NetMatchService::ServedReturnBaseLocked(uint64_t nowMs) const {
		// A returner arriving now is served the published base, unless it would take a new one.
		std::optional<uint64_t> served;
		const NetWorldCheckpointImage& image = m_WorldJoin.Image();
		if (image.IsValid() && !m_WorldJoin.IsPrivateMatch()) {
			// A world's joiner starts from the published image while its tail is still in memory; an older one waits for the capture its join asks for.
			if (m_WorldJoin.Tail().Count() != 0 && image.tick + 1 >= m_WorldJoin.Tail().FirstFrame()) served = image.tick;
		} else if (image.IsValid()) {
			const bool fresh = m_PrivateImageTakenMs != 0 && nowMs - m_PrivateImageTakenMs < c_PrivateImageMinIntervalMs;
			if (!m_PrivateImageRecapture && (fresh || !PrivateBaseRefreshDue(true, 0, image.tick, SteadyCaptureMs(m_PrivateCaptureCosts)))) served = image.tick;
		}
		return served;
	}

	uint64_t NetMatchService::ReturnHistoryFloor(uint64_t tick, std::optional<uint64_t> servedBaseTick, const std::vector<NetWorldJoinSession>& sessions,
	                                             const std::map<uint8_t, NetGameSeatHold>& holds, double tickMs, uint8_t returnWindowMinutes) {
		uint64_t floor = tick + 1;
		if (servedBaseTick) floor = std::min(floor, *servedBaseTick + 1);
		// Every return under way reads the tail on from the last frame it applied.
		for (const NetWorldJoinSession& session: sessions)
			if (session.phase != NetWorldJoinPhase::Active && session.snapshotTick != 0) floor = std::min(floor, session.acknowledgedThrough + 1);
		// A held seat may come back holding its own state from just before its hold for as long as the host's return window keeps it.
		const uint64_t window = ReturnWindowFrames(returnWindowMinutes, tickMs);
		const uint64_t skew = NetLockstepCodec::c_MaxFutureFrameSkew;
		for (const auto& [peer, hold]: holds)
			if (tick < hold.cutoffFrame + window) floor = std::min(floor, hold.cutoffFrame > skew ? hold.cutoffFrame - skew : 1);
		return floor;
	}

	uint64_t NetMatchService::ReturnWindowFrames(uint8_t minutes, double tickMs) {
		return static_cast<uint64_t>(std::ceil(minutes * 60000.0 / (tickMs > 0.0 ? tickMs : 1000.0 / 60.0)));
	}

	uint64_t NetMatchService::EffectiveJoinRetention(const NetJoinHistoryPolicy& policy, double tickMs, bool* byWindow) {
		// A return inside the agreed window resumes from frames reaching back to its hold's floor, so the history never holds less.
		const uint64_t window = policy.retainFrames == 0 ? 0 : ReturnWindowFrames(policy.returnWindowMinutes, tickMs) + NetLockstepCodec::c_MaxFutureFrameSkew + 1;
		if (byWindow) *byWindow = window > policy.retainFrames;
		return std::max(policy.retainFrames, window);
	}

	uint64_t NetMatchService::StepJoinHistory(NetWorldJoinHost& host, uint64_t tick, std::optional<uint64_t> servedBaseTick, const std::map<uint8_t, NetGameSeatHold>& holds, double tickMs,
	                                          const NetJoinHistoryPolicy& policy, std::vector<NetJoinHistoryEnd>* ends) {
		NetWorldFrameLog& tail = host.Tail();
		tail.SetJournalRetention(EffectiveJoinRetention(policy, tickMs, nullptr));
		const bool journalFailed = tail.JournalFailed();
		if (std::string why; tail.TakeJournalFailure(why))
			System::PrintDiagnosticLine(std::format("[round-history] journal failed tick={} file={} reason={} memory_frames={}..{}", tick, tail.JournalFileName(), why, tail.FirstFrame(), tail.LastFrame()));
		// A reader the history can no longer serve, or one that trails the round past the lag limit without gaining on it and with no join
		// deadline to end it, would hold the journal open for itself alone: its bootstrap ends here, and a match's returning seat comes back
		// on a fresh image. A bootstrap still waiting for its transfer is bound to the next image instead.
		std::vector<NetJoinHistoryEnd> ending;
		std::vector<NetPeerId> lagging;
		for (const NetWorldJoinSession& session: host.Sessions())
			if (StreamsTail(session) && (session.spectator || host.IsPrivateMatch())) lagging.push_back(session.connection);
		std::erase_if(lagging, [&](NetPeerId connection) { return !host.TrailsPastLagLimit(connection, tick, policy.lagLimitFrames); });
		for (const NetWorldJoinSession& session: host.Sessions()) {
			if (session.phase == NetWorldJoinPhase::Active || session.phase == NetWorldJoinPhase::Failed || session.snapshotTick == 0) continue;
			if (session.phase == NetWorldJoinPhase::SnapshotTransfer && !session.transferStarted) continue;
			const uint64_t needed = session.acknowledgedThrough + 1;
			const uint64_t trail = tick > session.acknowledgedThrough ? tick - session.acknowledgedThrough : 0;
			const char* reason = nullptr;
			if (needed <= tick && !tail.Covers(needed)) reason = journalFailed ? "journal" : "history";
			else if (std::find(lagging.begin(), lagging.end(), session.connection) != lagging.end()) reason = "lag";
			if (reason == nullptr) continue;
			NetJoinHistoryEnd end;
			end.connection = session.connection;
			end.rebase = host.IsPrivateMatch() && !session.spectator;
			end.receipt = std::format("[round-history] reader ended connection={} peer={} kind={} reason={} applied={} horizon={} trail_frames={} lag_limit_frames={} first_servable={} action={} seat={}",
			                          session.connection, static_cast<int>(session.assignedPeerId), session.spectator ? "watcher" : session.returnsToHeldSeat ? "returner" : "joiner", reason,
			                          session.acknowledgedThrough, tick, trail, policy.lagLimitFrames, tail.FirstServableFrame(), end.rebase ? "rebased" : "closed",
			                          session.returnsToHeldSeat ? "held" : "none");
			ending.push_back(std::move(end));
		}
		for (const NetJoinHistoryEnd& end: ending) {
			host.EndBootstrap(end.connection, "the round's history no longer serves it");
			System::PrintDiagnosticLine(end.receipt);
		}
		if (tail.JournalFailed() && tail.ReopenJournal(tick))
			System::PrintDiagnosticLine(std::format("[round-history] journal reopened tick={} file={} first={} reopens={}", tick, tail.JournalFileName(), tail.FirstServableFrame(), tail.JournalReopens()));
		uint64_t floor = ReturnHistoryFloor(tick, servedBaseTick, host.Sessions(), holds, tickMs, policy.returnWindowMinutes);
		// Nothing older than the history can still serve is kept for a return.
		if (const uint64_t first = tail.FirstServableFrame(); first != 0) floor = std::max(floor, std::min(first, tick + 1));
		tail.PruneJournalBefore(floor);
		if (ends) ends->insert(ends->end(), std::make_move_iterator(ending.begin()), std::make_move_iterator(ending.end()));
		return floor;
	}

	NetJoinHistoryPolicy NetMatchService::JoinHistoryPolicyFromSettings(double tickMs) {
		const double tick = std::isfinite(tickMs) && tickMs > 0 ? tickMs : 1000.0 / 60.0;
		const auto frames = [&](int seconds) { return static_cast<uint64_t>(std::llround(seconds * 1000.0 / tick)); };
		NetJoinHistoryPolicy policy;
		policy.retainFrames = frames(g_SettingsMan.GetNetworkHostJoinHistorySeconds());
		policy.lagLimitFrames = frames(g_SettingsMan.GetNetworkHostJoinLagSeconds());
		return policy;
	}

	void NetMatchService::PruneReturnHistory(uint64_t tick) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_Coordinator || !m_WorldJoin.IsConfigured()) return;
		NetLockstepPlaneGuard plane;
		const double tickMs = m_Coordinator->GetConfig().simTickMs;
		std::vector<NetJoinHistoryEnd> ends;
		NetJoinHistoryPolicy policy = JoinHistoryPolicyFromSettings(tickMs);
		policy.returnWindowMinutes = m_Coordinator->GetConfig().matchConfig.returnWindowMinutes;
		const uint64_t floor = StepJoinHistory(m_WorldJoin, tick, ServedReturnBaseLocked(SteadyNowMs()), m_Coordinator->HeldTransactions(), tickMs, policy, &ends);
		(void)EffectiveJoinRetention(policy, tickMs, &m_JoinRetentionByWindow);
		m_LastReturnHistoryFloor = floor;
		m_Coordinator->SetReturnHistoryFloor(floor);
		// A returning seat goes back for a fresh image on its ticket; any other reader is told the world moved on past it.
		if (m_Session)
			for (const NetJoinHistoryEnd& end: ends) m_Session->DisconnectReadyPeer(end.connection, NetRejectReason::HostNotAccepting, end.rebase ? c_ImageRejoinDetail : c_HistoryPassedDetail);
	}

	bool NetMatchService::CaptureFullStateHash(uint64_t tick, uint64_t round, const std::string& dumpDirectory, const std::string& label) {
		{
			NetLockstepPlaneGuard plane;
			if (m_Coordinator) m_Coordinator->BeginSynchronizedCapture(tick);
		}
		const auto began = std::chrono::steady_clock::now();
		const bool taken = g_ActivityMan.CaptureFullStateHash(tick, round, dumpDirectory, label);
		const double costMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
		{
			NetLockstepPlaneGuard plane;
			if (m_Coordinator) m_Coordinator->CompleteSynchronizedCapture(tick, costMs);
		}
		return taken;
	}

	void NetMatchService::AutosaveAtTickBoundary(uint64_t tick) {
		std::vector<CheckpointNote> applied;
		for (const auto& [sender, checkpoint]: ScenarioRunner::TakeAppliedCheckpoints()) applied.push_back({sender, checkpoint.kind, checkpoint.tick});
		// A segment held for its checkpoint opens the moment the archive thread has named the digest.
		std::vector<uint64_t> refused;
		const std::vector<uint64_t> finished = TakeAutosaveVerdicts(&refused);
		if (ScenarioRunner::HasPendingLockstepWorldSegment()) SealWorldReplaySegment();
		AppendCommittedJoinFrame(tick);
		if (!ScenarioRunner::IsLockstepControllerSyncActive() || m_AutosaveMatchId.empty() || !m_Coordinator) return;
		AutosaveTickInput input;
		input.tick = tick;
		input.now = g_TimerMan.GetSimTimeTicks();
		input.unwritten = g_ActivityMan.UnwrittenAutosaves();
		input.applied = std::move(applied);
		input.finished = finished;
		input.refused = std::move(refused);
		input.localPeer = GetLocalPeerId();
		if (m_IsHost) {
			input.writers = CheckpointWriters(tick);
			input.lead = static_cast<uint16_t>(m_Coordinator->InputDelayAt(GetLocalPeerId(), tick) + 2);
			// An activation told to a returner but not yet scheduled is decided in its last frames: a capture named at or before it
			// would hold those frames open and push the activation a lead later, onto the same phase of the next capture.
			const uint64_t announced = m_AnnouncedActivationTick.load(std::memory_order_relaxed);
			input.activationPending = m_Coordinator->HasPendingSeatActivation() || (announced != 0 && announced >= tick + input.lead);
			input.ownSeatHeld = m_Coordinator->IsOwnHostSeatHeld();
			const auto& start = m_Coordinator->GetAgreedStartRecord();
			input.startupPending = start && tick < start->agreedFirstFrame;
			input.manualRequested = m_ManualSaveAsked.exchange(false);
			input.hostProvisional = m_Coordinator->IsHostProvisional();
		}
		AutosaveTickOutput output = StepAutosaveSchedule(input);
		if (output.manualRefused) {
			ScenarioRunner::PushNetUiToast("match_save", "Match not saved: the connection to the other players is lost");
			m_ManualSaveWaitShown = false;
		}
		if (output.manualPending) {
			m_ManualSaveAsked = true;
			if (!m_ManualSaveWaitShown) ScenarioRunner::PushNetUiToast("match_save", "Saving at the next safe tick");
			m_ManualSaveWaitShown = true;
		} else if (input.manualRequested) {
			m_ManualSaveWaitShown = false;
		}
		if (output.manualSaved) {
			const bool saved = m_ManualSaveFailure.empty();
			const std::string peers = output.manualReported == 1 ? "1 peer" : std::to_string(output.manualReported) + " peers";
			const std::string line = !saved ? "Match not saved: " + m_ManualSaveFailure
			                                : output.manualReported == output.manualWriters ? "Match saved (" + peers + ")"
			                                                                                : "Match saved (" + std::to_string(output.manualReported) + " of " + std::to_string(output.manualWriters) + " peers)";
			ScenarioRunner::PushNetUiToast("match_save", line);
			System::PrintDiagnosticLine(std::format("[autosave] manual save {} tick={} writers={}/{}", saved ? "saved" : "failed", m_ManualSaveTakenTick, output.manualReported, output.manualWriters));
			m_ManualSaveFailed = !saved;
			m_ManualSaveBusy = m_ManualSaveAsked.load();
		}
		if (output.capture) {
			const bool joinCapture = IsJoinCaptureTick(tick);
			// A peer replaying its catch-up is not live at this tick, and the host does not wait on it.
			bool taken = false;
			if (!ScenarioRunner::WorldCatchUpActive() && g_ActivityMan.ActivityRunning()) {
				if (m_Coordinator) m_Coordinator->BeginSynchronizedCapture(tick);
				const auto captureBegan = std::chrono::steady_clock::now();
				CrossCaptureBarrierFromEnvironment("capture_announced", tick, m_AutosaveIdentity.roundId);
				m_AutosaveIdentity.savedByHost = output.manual;
				taken = SaveStampedAutosave(tick);
				m_AutosaveIdentity.savedByHost = false;
				const double captureMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - captureBegan).count();
				if (m_Coordinator) m_Coordinator->CompleteSynchronizedCapture(tick, taken ? std::max(g_ActivityMan.LastAutosaveCaptureMs(), captureMs) : captureMs);
			}
			if (output.manual) {
				System::PrintDiagnosticLine(std::format("[autosave] manual save {} tick={}", taken ? "taken" : "not taken", tick));
				if (m_IsHost) {
					m_ManualSaveTakenTick = tick;
					if (!taken) m_ManualSaveFailure = "the capture was not taken";
				}
			}
			if (taken) {
				if (input.unwritten > 0) System::PrintDiagnosticLine(std::format("[autosave] named tick={} taken with unwritten={}", tick, input.unwritten));
				// The capture a returning seat's base was asked of: its metadata is read at this tick's end, its archive from the writer.
				if (m_IsHost && joinCapture && m_WorldJoin.IsPrivateMatch() && m_PrivateBaseRequested) {
					m_PrivateBaseRequested = false;
					m_PrivateBaseTick = tick;
				}
				// The queue is not the verdict: the worker walks the graph off this thread and may still refuse.
				m_AwaitedAutosaves.push_back(AwaitedAutosave{tick, joinCapture});
				// The segment holds this tick's frames from here; it opens when the archive validates and is
				// dropped with it when the capture is refused.
				RollWorldReplaySegment(tick);
				// The round's history at each save: what this peer keeps of it in memory and on disk, beside the bounds that keep it flat.
				if (m_WorldJoin.IsConfigured()) {
					const NetWorldFrameLog& tail = m_WorldJoin.Tail();
					const NetWorldFrameLog::JournalStats journal = tail.GetJournalStats();
					uint64_t journalBound = m_LastReturnHistoryFloor != 0 && journal.last >= m_LastReturnHistoryFloor ? journal.last + 1 - m_LastReturnHistoryFloor + NetWorldFrameLog::c_JournalSegmentFrames : 0;
					// The host's retained history caps it whatever the floor reads.
					if (tail.JournalRetention() != 0) journalBound = std::min(journalBound, tail.JournalRetention() + NetWorldFrameLog::c_JournalSegmentFrames);
					System::PrintDiagnosticLine(std::format("[round-history] tick={} memory_frames={} memory_bytes={} memory_bound_frames={} memory_bound_bytes={} journal_files={} journal_bytes={} "
					                                        "journal_first={} journal_last={} floor={} journal_bound_frames={} journal_index_bytes={} journal_cached_reads={} journal_cached_read_bytes={} "
					                                        "journal_retain_frames={} journal_failed={} journal_reopens={} journal_retain_by={}",
					                                        tick, tail.Count(), tail.Bytes(), tail.MaxFrames(), tail.MaxBytes(), journal.files, journal.bytes, journal.first, journal.last,
					                                        m_LastReturnHistoryFloor, journalBound, journal.indexBytes, journal.cachedReads, journal.cachedReadBytes, tail.JournalRetention(),
					                                        tail.JournalFailed(), tail.JournalReopens(), m_JoinRetentionByWindow ? "return_window" : "world_history"));
				}
				if (m_WorldJoin.IsConfigured()) {
					// The image is published when the writer thread has finished this archive, from the pump.
					std::ostringstream line;
					line << "[net-world] metrics " << m_WorldJoin.Metrics().BuildReportJson();
					System::PrintDiagnosticLine(line.str());
				}
			} else {
				// A capture this peer did not take holds nothing, and says so at once.
				System::PrintDiagnosticLine(std::format("[autosave] named tick={} not taken: catch_up={} running={}", tick, ScenarioRunner::WorldCatchUpActive(), g_ActivityMan.ActivityRunning()));
				output.send.push_back({0, NetGameCheckpoint::Missed, tick});
			}
		}
		for (const CheckpointNote& note: output.send) {
			if (note.kind == NetGameCheckpoint::Capture) System::PrintDiagnosticLine(std::format("[autosave] named tick={} at={} writers={}", note.tick, tick, input.writers.size()));
			if (note.kind == NetGameCheckpoint::ManualCapture) {
				System::PrintDiagnosticLine(std::format("[autosave] manual save named tick={} at={} writers={}", note.tick, tick, input.writers.size()));
				m_ManualSaveFailure.clear();
				m_ManualSaveBusy = true;
			}
			(void)ScenarioRunner::SubmitCheckpoint(NetGameCheckpoint{note.kind, note.tick});
		}
	}

	bool NetMatchService::RequestManualSave() {
		bool host = false, running = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			host = m_IsHost;
			running = m_State == NetMatchServiceState::Running && !m_AutosaveMatchId.empty();
		}
		// A client's own save would be a world no other peer holds.
		if (!host) {
			ScenarioRunner::PushNetUiToast("match_save", "The host saves the match");
			return false;
		}
		if (!running) {
			ScenarioRunner::PushNetUiToast("match_save", "The match cannot be saved until it runs");
			return false;
		}
		m_ManualSaveAsked = true;
		m_ManualSaveBusy = true;
		ScenarioRunner::PushNetUiToast("match_save", ScenarioRunner::IsLockstepPaused() ? "Saving when the match resumes" : "Saving...");
		System::PrintDiagnosticLine("[autosave] manual save asked");
		return true;
	}

	NetMatchService::MatchSaveRow NetMatchService::GetMatchSaveRow() const {
		MatchSaveRow row;
		bool host = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			host = m_IsHost;
			row.enabled = host && m_State == NetMatchServiceState::Running && !m_AutosaveMatchId.empty();
		}
		const int64_t saved = m_LastMatchSaveTime.load();
		const std::string at = saved == 0 ? "" : System::LocalTimeText(static_cast<std::time_t>(saved), "%H:%M");
		if (!host) {
			row.hint = at.empty() ? "The host saves the match" : "The host saves the match - last saved at " + at;
		} else if (m_ManualSaveBusy) {
			row.hint = "Saving...";
		} else if (m_ManualSaveFailed) {
			row.hint = at.empty() ? "The last save failed" : "The last save failed - last saved at " + at;
		} else {
			row.hint = at.empty() ? "Not saved yet" : "Last saved at " + at;
		}
		return row;
	}

	void NetMatchService::RollWorldReplaySegment(uint64_t tick) {
		// Only a recording world cuts segments: an ordinary match keeps its one file, and a world that
		// was never asked to record writes nothing.
		if (!m_WorldJoin.IsConfigured() || m_WorldIdentity.worldId.empty() || tick == 0) return;
		if (!ScenarioRunner::IsLockstepReplayRecording() && !ScenarioRunner::HasPendingLockstepWorldSegment()) return;
		NetWorldSegmentHeader header;
		header.worldId = m_WorldIdentity.worldId;
		header.tick = tick;
		header.round = m_AutosaveIdentity.roundId;
		header.boot = m_WorldIdentity.boot;
		const std::string path = AutosaveStore::SegmentPath(AutosaveStore::Directory(), header.worldId, tick).string();
		ScenarioRunner::ArmLockstepWorldSegment(header, path);
	}

	NetMatchService::RoundRecordingPlan NetMatchService::PlanRoundRecording(bool persistentWorld, bool worldIdentityValid,
	                                                                       uint64_t resumeTick, const std::string& resumeDigest,
	                                                                       uint64_t healTick, const std::string& healDigest) {
		RoundRecordingPlan plan;
		if (!persistentWorld || !worldIdentityValid) {
			return plan;
		}
		// A resumed round names its own checkpoint; a healed one rewound to the checkpoint the host
		// named, and the stretch from there to the next roll has to chain too.
		const uint64_t tick = resumeTick != 0 ? resumeTick : healTick;
		const std::string& digest = resumeTick != 0 ? resumeDigest : healDigest;
		if (tick == 0 || digest.empty()) {
			return plan;
		}
		plan.segment = true;
		plan.tick = tick;
		plan.digest = digest;
		return plan;
	}

	void NetMatchService::SealWorldReplaySegment() {
		const uint64_t tick = ScenarioRunner::GetPendingLockstepWorldSegmentTick();
		if (tick == 0 || m_AutosaveMatchId.empty()) return;
		const std::optional<AutosaveDescriptor> validated = AutosaveStore::NewestValidated(m_AutosaveMatchId);
		if (!validated || validated->savedTick != tick || validated->worldStructureHash.empty()) return;
		std::string error;
		if (!ScenarioRunner::SealLockstepWorldSegment(validated->worldStructureHash, &error)) {
			{
				std::ostringstream line;
				line << "[net-world] segment not opened: " << error;
				System::PrintDiagnosticLine(line.str());
			}
		}
	}

	void NetMatchService::SealPendingWorldSegmentAtEnd() {
		// The round's last checkpoint still waits on its archive when the round ends; the recorder holds the ticks since it, and
		// they are the segment's. Waiting out the writer here, at the round's end, opens that segment instead of losing them.
		if (!ScenarioRunner::HasPendingLockstepWorldSegment()) return;
		const uint64_t tick = ScenarioRunner::GetPendingLockstepWorldSegmentTick();
		g_ActivityMan.WaitForAutosaveTasks();
		SealWorldReplaySegment();
		if (ScenarioRunner::HasPendingLockstepWorldSegment())
			ScenarioRunner::DropPendingLockstepWorldSegment("the checkpoint at tick " + std::to_string(tick) + " never validated before the round ended");
	}

	bool NetMatchService::SaveStampedAutosave(uint64_t tick) {
		if (tick != static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount())) {
			System::PrintDiagnosticLine(std::format("[autosave] capture refused: requested tick={} world tick={}", tick, g_TimerMan.GetSimUpdateCount()));
			return false;
		}
		// The tick is complete, so the agreed lockstep state of THIS tick is what a restart needs; it
		// is read once, here, through the same reader the heal's snapshot capture uses.
		m_AutosaveIdentity.sideState = ScenarioRunner::CaptureAgreedSideState();
		m_AutosaveIdentity.migrationGen = m_MigrationGeneration;
		if (m_IsHost && m_MatchConfig.persistentWorld) {
			System::PrintDiagnosticLine(std::format("[autosave] agreed match={} tick={} state={}", m_AutosaveMatchId, tick,
			                                       nlohmann::json(AutosaveStore::RenderSideState(m_AutosaveIdentity.sideState)).dump()));
			// A world joiner starts on this tick's lockstep state and seats: a hold's handoffs before the image are not in its tail.
			NetResyncState state;
			std::vector<uint8_t> side;
			std::string error;
			if (ScenarioRunner::CaptureNetResyncState(tick, state, &error)) {
				state.pendingInputs.clear(); state.pendingCommands.clear(); state.pendingPlayerBindings.clear(); state.admittedReseats.clear();
				if (!NetResyncCodec::Encode(state, {0}, side, &error)) side.clear();
			}
			NetWorldCheckpointImage seats;
			if (!side.empty() && m_Coordinator) {
				NetLockstepPlaneGuard plane;
				CaptureWorldImageSeats(*m_Coordinator, tick, seats);
				seats.sideState = ResumeHex(side);
			}
			if (!seats.sideState.empty() && !seats.heldState.empty()) {
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_WorldImageStates[tick] = std::move(seats);
				// Captures that never publish do not pile up.
				while (m_WorldImageStates.size() > 8) m_WorldImageStates.erase(m_WorldImageStates.begin());
			} else {
				System::PrintDiagnosticLine(std::format("[net-world] no lockstep state for the image at tick {}: {}", tick,
				                                        !error.empty() ? error : m_Coordinator ? "its seats did not encode" : "there is no round"));
			}
		}
		return g_ActivityMan.SaveAutosaveSnapshot(m_AutosaveMatchId, tick, m_AutosaveIdentity);
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

	void NetMatchService::CaptureWorldImageSeats(const NetLockstepCoordinator& coordinator, uint64_t tick, NetWorldCheckpointImage& image) {
		// The tail after the image carries every transition from tick + 1 on; the ones at or before it govern the frames it starts.
		const uint8_t host = coordinator.GetHostPeerId();
		const auto& config = coordinator.GetConfig();
		const auto leaves = coordinator.GetPeerLeaveFrames();
		const auto holds = coordinator.HeldTransactions();
		NetLockstepFrame seats;
		seats.targetFrame = tick;
		seats.roundId = coordinator.GetRoundId();
		std::map<uint8_t, uint64_t> heldFrom;
		for (uint8_t peer = 1; peer <= config.peerCount; ++peer) {
			if (!coordinator.IsSeatUnderAI(peer, tick)) continue;
			const auto held = holds.find(peer);
			NetGameSeatHold hold = held != holds.end() && held->second.cutoffFrame <= tick ? held->second : NetGameSeatHold{peer};
			if (hold.cutoffFrame == 0) {
				// A seat held from the round's start has no transaction: it is held from its leave, or from this tick at the latest.
				const auto leave = leaves.find(peer);
				hold.authorityGeneration = config.migrationGeneration;
				hold.seatIncarnation = config.peerIncarnations.contains(peer) ? config.peerIncarnations.at(peer) : 0;
				hold.cutoffFrame = leave != leaves.end() && leave->second <= tick ? leave->second : tick;
			}
			heldFrom[peer] = hold.cutoffFrame;
			seats.commands.push_back({host, hold});
		}
		// A return older than the seat's hold is history; a newer one at or before the tick may still be inside its neutral gap.
		for (const auto& [peer, reclaim]: coordinator.ReclaimTransactions())
			if (reclaim.activationFrame <= tick && (!heldFrom.contains(peer) || heldFrom.at(peer) < reclaim.activationFrame)) seats.commands.push_back({host, reclaim});
		image.departedPeers.clear();
		for (const auto& [peer, frame]: leaves) if (frame <= tick) image.departedPeers[peer] = frame;
		image.authorityGeneration = config.migrationGeneration;
		image.authorityPeerId = host;
		std::vector<uint8_t> bytes;
		image.heldState = EncodeCommittedJoinFrame(seats, bytes) ? ResumeHex(bytes) : std::string();
	}

	bool NetMatchService::AdoptWorldImageSeats(const NetWorldCheckpointImage& image, uint8_t peerCount, NetWorldCatchUpClient& catchUp, std::string* error) {
		// A joiner started without the image's seats would answer a held seat as played and resolve its units to the wrong producer.
		std::vector<uint8_t> bytes;
		NetLockstepFrame seats;
		if (image.sideState.empty() || image.heldState.empty() || !ResumeBytes(image.heldState, bytes) || !DecodeCommittedJoinFrame(bytes, seats, error) ||
		    seats.targetFrame != image.tick) {
			if (error) *error = "the world image carries no seat state for its tick " + std::to_string(image.tick) + (error->empty() ? "" : ": " + *error);
			return false;
		}
		if (image.authorityPeerId == 0 || image.authorityPeerId > peerCount) {
			if (error) *error = "the world image names authority peer " + std::to_string(image.authorityPeerId) + " outside its " + std::to_string(peerCount) + " seats";
			return false;
		}
		std::map<uint8_t, NetGameSeatHold> holds;
		std::map<uint8_t, NetGameSeatReclaim> reclaims;
		for (const NetGameCommand& command: seats.commands) {
			const auto* hold = std::get_if<NetGameSeatHold>(&command.payload);
			const auto* reclaim = std::get_if<NetGameSeatReclaim>(&command.payload);
			const uint8_t peer = hold ? hold->peerId : reclaim ? reclaim->peerId : 0;
			const uint64_t frame = hold ? hold->cutoffFrame : reclaim ? reclaim->activationFrame : 0;
			if (command.senderPeerId != image.authorityPeerId || peer == 0 || peer > peerCount || frame == 0 || frame > image.tick) {
				if (error) *error = "the world image's seat state names a transition outside its round: peer " + std::to_string(peer) + " at " + std::to_string(frame);
				return false;
			}
			if (hold) holds[peer] = *hold;
			else reclaims[peer] = *reclaim;
		}
		for (const auto& [peer, frame]: image.departedPeers) {
			if (peer == 0 || peer > peerCount || frame > image.tick) {
				if (error) *error = "the world image names a departure outside its round: peer " + std::to_string(peer) + " at " + std::to_string(frame);
				return false;
			}
		}
		catchUp.initialHolds = std::move(holds);
		catchUp.initialReclaims = std::move(reclaims);
		catchUp.initialPeerLeaves = image.departedPeers;
		catchUp.authorityGeneration = image.authorityGeneration;
		catchUp.authorityPeerId = image.authorityPeerId;
		return true;
	}

	void NetMatchService::SeedWorldReplaySeats(const NetWorldCatchUpClient& catchUp, NetLockstepConfig& config) {
		config.initialPeerLeaves = catchUp.initialPeerLeaves;
		config.initialSeatHolds = catchUp.initialHolds;
		config.initialSeatReclaims = catchUp.initialReclaims;
		for (const auto& [peer, hold]: catchUp.initialHolds) config.peerIncarnations[peer] = std::max(config.peerIncarnations[peer], hold.seatIncarnation);
		for (const auto& [peer, reclaim]: catchUp.initialReclaims) config.peerIncarnations[peer] = std::max(config.peerIncarnations[peer], reclaim.seatIncarnation);
	}

	void NetMatchService::AdoptWorldReplaySeats(NetLockstepCoordinator& live, const NetLockstepCoordinator& replay, uint64_t activationTick) {
		// The round was configured when its handshake began; a hold or return the replay took since then is the round's too.
		if (activationTick != 0) live.AdoptReplayedSeatTransitions(replay, activationTick - 1);
	}

	void NetMatchService::PublishFinishedWorldJoinImage() {
		if (m_WorldJoin.IsPrivateMatch() && m_PrivateBasePending) {
			const std::optional<ActivityMan::CompletedAutosave> entry = g_ActivityMan.LastCompletedAutosave();
			if (!entry || entry->tick != m_PrivateBasePending->tick || entry->archive == nullptr || entry->archive->empty() || entry->bytes != entry->archive->size()) return;
			NetWorldCheckpointImage image = std::move(*m_PrivateBasePending);
			m_PrivateBasePending.reset();
			if (image.round != m_WorldJoin.TailRound()) {
				System::PrintDiagnosticLine("[net-match] a private base of round " + std::to_string(image.round) + " is not published into round " + std::to_string(m_WorldJoin.TailRound()));
				return;
			}
			image.path = entry->path; image.bytes = entry->bytes; image.digest = entry->digest; image.captureMs = g_ActivityMan.LastAutosaveCaptureMs();
			m_WorldJoinImageArchive = entry->archive;
			m_WorldJoinImageDigest = image.digest;
			m_PrivateImageLastCaptureMs = image.captureMs;
			if (!m_PrivateCaptureCold) {
				m_PrivateCaptureCosts.push_back(image.captureMs);
				if (m_PrivateCaptureCosts.size() > 3) m_PrivateCaptureCosts.pop_front();
			}
			System::PrintDiagnosticLine("[net-match] private base published from the announced capture at tick " + std::to_string(image.tick) + " bytes=" + std::to_string(image.bytes));
			m_WorldJoin.PublishImage(image);
			return;
		}
		if (m_WorldJoin.IsPrivateMatch()) {
			if (!m_PrivateImageTask.valid() || m_PrivateImageTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
			auto ready = m_PrivateImageTask.get();
			if (ready.error.empty() && ready.image.IsValid() && ready.image.round != m_WorldJoin.TailRound()) {
				System::PrintDiagnosticLine("[net-match] a private base of round " + std::to_string(ready.image.round) + " is not published into round " + std::to_string(m_WorldJoin.TailRound()));
				return;
			}
			if (!ready.error.empty() || !ready.image.IsValid()) {
				m_PrivateJoinError = ready.error.empty() ? "the private checkpoint is invalid" : ready.error;
				{
					std::ostringstream line;
					line << "[net-match] private checkpoint unavailable: " << m_PrivateJoinError;
					System::PrintDiagnosticLine(line.str());
				}
				return;
			}
			m_WorldJoinImageArchive = std::move(ready.archive);
			m_WorldJoinImageDigest = ready.image.digest;
			m_PrivateImageLastCaptureMs = ready.image.captureMs;
			if (!m_PrivateCaptureCold) {
				m_PrivateCaptureCosts.push_back(ready.image.captureMs);
				if (m_PrivateCaptureCosts.size() > 3) m_PrivateCaptureCosts.pop_front();
			}
			m_WorldJoin.PublishImage(ready.image);
			return;
		}
		if (!m_WorldJoin.IsConfigured()) {
			return;
		}
		// Test lever: the world keeps serving its first capture, so a rejoin catches up across everything since it.
		static const bool firstImageOnly = std::getenv("CC_TEST_WORLD_JOIN_FIRST_IMAGE") != nullptr;
		if (firstImageOnly && m_WorldJoin.Image().IsValid()) {
			return;
		}
		const std::optional<ActivityMan::CompletedAutosave> entry = g_ActivityMan.LastCompletedAutosave();
		if (!entry || entry->tick <= m_WorldJoin.Image().tick) {
			return;
		}
		NetWorldCheckpointImage image = WorldImageFromAutosave(*entry, m_WorldIdentity, m_MatchConfig,
		                                                      m_WorldJoin.Membership().Revision(), g_ActivityMan.LastAutosaveCaptureMs());
		if (!image.IsValid()) {
			return;
		}
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			const auto state = m_WorldImageStates.find(image.tick);
			if (state == m_WorldImageStates.end()) {
				// An archive and the lockstep state of its tick are one image: without the state no joiner may start on it, and the next
				// bootstrap asks for a capture of its own.
				if (m_WorldImageRefusedTick != image.tick) {
					m_WorldImageRefusedTick = image.tick;
					System::PrintDiagnosticLine("[net-world] the image at tick " + std::to_string(image.tick) + " is not published: no lockstep state was captured at its tick");
				}
				if (m_WorldCaptureRequestedTick != 0 && m_WorldCaptureRequestedTick <= image.tick) m_WorldCaptureRequestedTick = 0;
				return;
			}
			image.sideState = state->second.sideState;
			image.heldState = state->second.heldState;
			image.departedPeers = state->second.departedPeers;
			image.authorityGeneration = state->second.authorityGeneration;
			image.authorityPeerId = state->second.authorityPeerId;
			m_WorldImageStates.erase(m_WorldImageStates.begin(), m_WorldImageStates.upper_bound(image.tick));
		}
		// The buffer the writer produced is shared, not copied: every bootstrap ships these bytes.
		m_WorldJoinImageDigest = image.digest;
		m_WorldJoinImageArchive = entry->archive;
		m_WorldJoin.PublishImage(image);
		if (m_WorldCaptureRequestedTick != 0 && m_WorldCaptureRequestedTick <= image.tick) {
			// The capture this mark stood for has landed, so the next bootstrap may ask for its own.
			m_WorldCaptureRequestedTick = 0;
		}
		System::PrintDiagnosticLine("[net-world] offer " + EncodeWorldJoinOffer(image));
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

	uint64_t NetMatchService::ApplyWorldJoinReport(NetLobbySession& lobby, NetWorldJoinHost& host, const NetLobbySession::WorldJoinReport& report, NetPeerId connection, uint64_t nowFrame, uint64_t nowMs) {
		if (connection == c_InvalidNetPeerId) {
			if (report.kind == c_NetWorldReportCatchUp) lobby.NoteCatchUpReportDrop(report.fromPeer, "lobby peer has no bootstrap connection");
			return 0;
		}
		if (report.kind == c_NetWorldReportProgress) {
			const uint16_t received = static_cast<uint16_t>(report.value & 0xFFFFU);
			const uint16_t total = static_cast<uint16_t>(report.value >> 32);
			(void)host.NoteTransferProgress(connection, received, total);
			if (total != 0 && received >= total && host.Image().IsValid()) {
				(void)host.NoteTransferComplete(connection, host.Image().bytes, nullptr);
			}
		} else if (report.kind == c_NetWorldReportActivationAck) {
			host.AcknowledgeActivation(connection, report.value);
		} else if (report.kind == c_NetWorldReportDecline) {
			// A watcher that asked to stay one is skipped when a slot frees; it keeps its stream.
			(void)host.NoteSpectatorPreference(connection, report.value != 0);
		} else if (report.kind == c_NetWorldReportCatchUp) {
			const NetWorldJoinSession* prior = host.FindSession(connection);
			// A returner's report and the gate it met, when the gate changes or once a second of the round.
			const auto noteGate = [&](const char* gate, const std::string& detail) {
				const NetWorldJoinSession* session = host.TakeCatchUpGateToLog(connection, gate, nowFrame);
				if (!session) return;
				std::ostringstream line;
				line << "[net-world] catch-up gate peer=" << static_cast<int>(session->assignedPeerId) << " gate=" << (session->catchUpGate ? session->catchUpGate : "none") << " applied=" << report.value
				     << " acknowledged=" << session->acknowledgedThrough << " horizon=" << nowFrame << " work_ticks=" << report.workTicks << " closing_measured=" << session->closingMeasured
				     << " closing_rate=" << session->closingRate << " replay_ratio=" << session->headroom.Ratio() << " replay_ready=" << session->headroom.Ready()
				     << " rtt_ms=" << session->tailAckRttMs << " link_rtt_ms=" << session->tailLinkRttMs
				     << " reports_sent=" << lobby.GetStats().catchUpReportsSent << " reports_received=" << lobby.GetStats().catchUpReportsReceived
				     << " reports_refused=" << lobby.GetStats().catchUpReportsRefused << " reports_dropped=" << lobby.GetStats().catchUpReportsDropped << detail;
				System::PrintDiagnosticLine(line.str());
			};
			if ((host.IsPrivateMatch() || (prior && prior->returnsToHeldSeat)) && !host.NoteRejoinCapacity(connection, report.workTicks, report.workUs, report.sentThrough)) {
				noteGate("capacity-dropped", " work_us=" + std::to_string(report.workUs));
				return 0;
			}
			uint64_t activation = 0;
			const uint64_t previous = prior ? prior->acknowledgedThrough : 0;
			const uint64_t lastMs = prior ? prior->lastCatchUpReportMs : 0;
			const uint64_t ticks = report.value > previous ? report.value - previous : 0;
			const uint64_t elapsed = (lastMs != 0 && nowMs > lastMs) ? nowMs - lastMs : 1;
			std::string progressError;
			if (!host.NoteCatchUpProgress(connection, report.value, ticks, elapsed, nowFrame, &activation, &progressError)) noteGate("refused", " error=" + progressError);
			else noteGate(nullptr, "");
			host.NoteCatchUpClock(connection, nowMs);
			host.AcknowledgeTailDatagrams(connection, SteadyNowMs());
			if (activation != 0) {
				if (const NetWorldJoinSession* session = host.FindSession(connection); session) {
					(void)lobby.SendPayloadTo(WorldJoinLobbyPeer(*session), MakeWorldJoinReport(c_NetWorldReportActivate, activation), nullptr);
					std::ostringstream line;
					line << "[net-world] activation announced peer=" << static_cast<int>(session->assignedPeerId) << " at=" << activation
					     << " applied=" << report.value << " horizon=" << nowFrame << " replay_ticks=" << session->wallCatchUpTicks
					     << " replay_ms=" << session->wallCatchUpMs;
					System::PrintDiagnosticLine(line.str());
				}
			}
			return activation;
		}
		return 0;
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
		NoteDroppedLobbyEvents(events.size());
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
			// An announced activation is the round's epoch: from that frame every sender spells its
			// observation keys out again, so a member admitted there decodes them with an empty table.
			if (m_WorldJoin.IsPrivateMatch() && report.kind == c_NetWorldReportCatchUp) OpenInPlaceRejoinLocked(report, readyPeers, nowMs);
			const NetPeerId connection = ResolveWorldReportConnection(m_WorldJoin, readyPeers, report.fromPeer);
			if (m_HostGoodbyeSeen && m_CompletedRoundFinalFrame != 0)
				(void)m_WorldJoin.BeginFinalTail(connection, m_CompletedRoundFinalFrame);
			const auto* prior = m_WorldJoin.FindSession(connection);
			const uint64_t previous = prior ? prior->activationTick : 0;
			if (const uint64_t announced = ApplyWorldJoinReport(lobby, m_WorldJoin, report, connection, nowFrame, nowMs); announced != 0) {
				if (previous && previous != announced) m_Coordinator->MoveObservationEpoch(previous, announced);
				else m_Coordinator->SetObservationEpoch(announced);
			}
		}
	}

	bool NetMatchService::StartJoinerImageTransfer(const NetWorldJoinSession& session, std::string* error, bool* outUnstartable) {
		if (outUnstartable) *outUnstartable = false;
		if (m_WorldJoin.IsPrivateMatch() && m_PrivateImageTask.valid()) return false;
		if (m_WorldJoin.IsPrivateMatch() && !m_PrivateJoinError.empty()) {
			if (error) *error = m_PrivateJoinError;
			if (outUnstartable) *outUnstartable = true;
			return false;
		}
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
		if (!lobby.IsRemoteConnectionLobbyUp(lobbyPeer)) return false;
		// Once per bootstrap, not once per retry: the seat's config does not change while it waits.
		if (!session.matchConfigSent) {
			if (!lobby.SendMatchConfigTo(lobbyPeer)) return false;
			m_WorldJoin.NoteMatchConfigSent(session.connection);
		}
		// The bytes and their digest were read once, when the image was published; re-reading the
		// archive per pump is a sim-thread stall the world pays for every waiting bootstrap.
		if (m_WorldJoinImageArchive == nullptr || m_WorldJoinImageArchive->empty() || m_WorldJoinImageDigest != m_WorldJoin.Image().digest) {
			if (error) *error = "the published archive is missing or its digest does not match";
			m_WorldJoin.Metrics().NoteBootstrapStall();
			return false;
		}
		uint64_t lastCopied = m_WorldJoin.Image().tick;
		std::vector<uint8_t> blob;
		if (m_WorldJoin.IsPrivateMatch()) {
			auto task = m_PrivateJoinBlobs.find(session.connection);
			if (task == m_PrivateJoinBlobs.end()) {
				const auto image = m_WorldJoin.Image();
				const auto archive = m_WorldJoinImageArchive;
				task = m_PrivateJoinBlobs.emplace(session.connection, std::async(std::launch::async, [image, archive] {
					std::vector<uint8_t> encoded;
					EncodeWorldJoinImageBlob(image, *archive, {}, encoded, nullptr);
					return encoded;
				})).first;
			}
			if (task->second.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return false;
			try { blob = task->second.get(); } catch (const std::exception& exception) { m_PrivateJoinError = exception.what(); }
			m_PrivateJoinBlobs.erase(task);
			if (blob.empty()) {
				if (m_PrivateJoinError.empty()) m_PrivateJoinError = "the private checkpoint envelope could not be prepared";
				if (error) *error = m_PrivateJoinError;
				if (outUnstartable) *outUnstartable = true;
				return false;
			}
		} else {
			std::vector<std::vector<uint8_t>> tail;
			(void)m_WorldJoin.Tail().CopyFrom(m_WorldJoin.Image().tick + 1, 512, 1024ULL * 1024ULL, tail, &lastCopied);
			if (!EncodeWorldJoinImageBlob(m_WorldJoin.Image(), *m_WorldJoinImageArchive, tail, blob, error)) return false;
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

	bool NetMatchService::FindWorldCleanLeave(const std::vector<NetH4SeatStatus>& statuses, const NetWorldJoinHost& world, WorldCleanLeave& outLeave) {
		outLeave = WorldCleanLeave{};
		for (const NetH4SeatStatus& status: statuses) {
			// A dropped or reclaiming seat keeps its slot for its own holder; a committed one has one.
			if (status.stableSeat == 0 || status.committed || status.dropped || status.reclaiming) {
				continue;
			}
			const NetWorldSlot* slot = nullptr;
			for (const NetWorldSlot& candidate: world.Membership().Slots()) {
				// The seat the slot is bound to is the one binding between the two planes.
				if (candidate.held && candidate.stableSeat == status.stableSeat) {
					slot = &candidate;
					break;
				}
			}
			if (slot == nullptr) {
				continue;
			}
			for (const NetWorldJoinSession& session: world.Sessions()) {
				// The slot's CURRENT holder: a stale generation is somebody the world already let go.
				if (session.phase != NetWorldJoinPhase::Active || session.stableSeat != status.stableSeat ||
				    session.assignedPeerId != slot->peerId || session.holderGeneration != slot->generation) {
					continue;
				}
				outLeave.peerId = slot->peerId;
				outLeave.stableSeat = slot->stableSeat;
				outLeave.holderGeneration = slot->generation;
				outLeave.team = slot->team;
				outLeave.connection = session.connection;
				return true;
			}
			// A member seated in the lobby before the round started has no world session: the roster alone binds it, and
			// a slot no session claims is released once the roster stops seating its seat.
			if (std::none_of(world.Sessions().begin(), world.Sessions().end(), [&](const NetWorldJoinSession& session) {
				    return !session.spectator && session.assignedPeerId == slot->peerId;
			    })) {
				outLeave.peerId = slot->peerId;
				outLeave.stableSeat = slot->stableSeat;
				outLeave.holderGeneration = slot->generation;
				outLeave.team = slot->team;
				return true;
			}
		}
		return false;
	}

	bool NetMatchService::FindHeldWorldCleanLeave(const std::vector<NetH4SeatStatus>& statuses, const NetWorldJoinHost& world, const std::set<uint8_t>& aiHeldPeers,
	                                              WorldCleanLeave& outLeave) {
		outLeave = WorldCleanLeave{};
		for (const NetWorldSlot& slot: world.Membership().Slots()) {
			if (slot.stableSeat == 0 || !aiHeldPeers.contains(slot.peerId)) continue;
			const auto status = std::find_if(statuses.begin(), statuses.end(), [&](const NetH4SeatStatus& entry) { return entry.stableSeat == slot.stableSeat; });
			if (status == statuses.end() || status->committed || status->dropped || status->reclaiming || status->substituting) continue;
			outLeave.peerId = slot.peerId;
			outLeave.stableSeat = slot.stableSeat;
			outLeave.holderGeneration = slot.generation;
			outLeave.team = slot.team;
			for (const NetWorldJoinSession& session: world.Sessions()) {
				if (session.assignedPeerId == slot.peerId && !session.spectator) outLeave.connection = session.connection;
			}
			return true;
		}
		return false;
	}

	namespace {
		// The one seat-to-slot binding both planes read: the slot this admission seat holds, preferring
		// the held one. A bootstrap cancelled before it activated gave the slot back, and that is still
		// the slot SlotOfSeat hands its holder on a reclaim, so an unheld binding answers too.
		const NetWorldSlot* BoundWorldSlot(const NetWorldMembership& membership, uint16_t stableSeat) {
			if (stableSeat == 0) {
				return nullptr;
			}
			const NetWorldSlot* bound = nullptr;
			for (const NetWorldSlot& slot: membership.Slots()) {
				if (slot.stableSeat != stableSeat) {
					continue;
				}
				if (slot.held) {
					return &slot;
				}
				if (bound == nullptr) {
					bound = &slot;
				}
			}
			return bound;
		}
	} // namespace

	std::vector<uint8_t> NetMatchService::WorldReclaimHoldSlots(const std::vector<NetH4SeatStatus>& statuses, const NetWorldMembership& membership,
	                                                             const std::set<uint8_t>& aiHeldPeers) {
		std::vector<uint8_t> holds;
		for (const NetH4SeatStatus& status: statuses) {
			if (status.stableSeat == 0 || !(status.dropped || status.reclaiming)) {
				continue;
			}
			// The hold belongs on the slot the seat holds, not on the slot its lockstep id names.
			if (const NetWorldSlot* bound = BoundWorldSlot(membership, status.stableSeat)) {
				holds.push_back(bound->peerId);
			}
		}
		// A seat the AI holds is still its member's: its returner reclaims the slot, never watches.
		for (const NetWorldSlot& slot: membership.Slots()) {
			if (slot.held && aiHeldPeers.contains(slot.peerId) && std::find(holds.begin(), holds.end(), slot.peerId) == holds.end()) holds.push_back(slot.peerId);
		}
		return holds;
	}

	size_t NetMatchService::BindSeatedWorldMembers(const std::vector<NetH4SeatStatus>& statuses, const NetSeatRoster& roster, NetWorldMembership& membership) {
		size_t bound = 0;
		for (const NetH4SeatStatus& status: statuses) {
			if (status.stableSeat == 0 || !status.committed || status.closed || BoundWorldSlot(membership, status.stableSeat)) continue;
			// A seat coming in through the image takes its slot when its join begins.
			const NetRosterSeat* seat = roster.Find(NetRosterIdOf(status.stableSeat));
			if (!seat || seat->phase == NetSeatPhase::RejoinImage || seat->phase == NetSeatPhase::RejoinCatchUp) continue;
			const NetWorldSlot* slot = membership.SlotOfPeer(status.lockstepPeerId);
			if (slot && !slot->held && membership.Hold(status.lockstepPeerId, status.stableSeat, seat->name)) ++bound;
		}
		return bound;
	}

	namespace {
		/// The lockstep id an admission seat names by itself: a world seat bound to any other slot plays that slot instead.
		uint8_t SeatOwnLockstepId(const std::vector<NetH4SeatStatus>& statuses, uint16_t stableSeat) {
			for (const NetH4SeatStatus& status: statuses) if (status.stableSeat == stableSeat) return status.lockstepPeerId;
			return 0;
		}
	} // namespace

	bool NetMatchService::PromoteWorldWatcher(NetWorldJoinHost& world, NetReconnectHost& admission, uint64_t nowFrame, uint64_t* outActivation, NetPeerId* outPromoted) {
		// The roster takes the watcher's seat onto the slot first: the world seats it only on a binding every peer will read.
		const std::vector<NetH4SeatStatus> statuses = admission.GetSeatStatuses();
		return world.PromoteWaitingSpectator(nowFrame, outActivation, outPromoted, nullptr, [&](const NetWorldJoinSession& watcher, const NetWorldSlot& slot) {
			const uint8_t played = slot.peerId == SeatOwnLockstepId(statuses, watcher.stableSeat) ? 0 : slot.peerId;
			admission.NoteSeatSlot(watcher.stableSeat, played);
			const NetRosterSeat* seat = admission.GetRoster().Find(NetRosterIdOf(watcher.stableSeat));
			return seat != nullptr && seat->bindingRef == played;
		});
	}

	void NetMatchService::PublishWorldSeatSlots(NetReconnectHost& admission, const NetWorldMembership& membership) {
		// The world seats a seat on its slot; the roster is what every peer reads of it. An opened seat's binding went with its player.
		const std::vector<NetH4SeatStatus> statuses = admission.GetSeatStatuses();
		for (const NetH4SeatStatus& status: statuses) {
			const NetRosterSeat* seat = admission.GetRoster().Find(NetRosterIdOf(status.stableSeat));
			if (seat == nullptr || seat->owner == 0) continue;
			const NetWorldSlot* held = nullptr;
			for (const NetWorldSlot& slot: membership.Slots()) if (slot.held && slot.stableSeat == status.stableSeat) held = &slot;
			const uint8_t played = held != nullptr && held->peerId != status.lockstepPeerId ? held->peerId : 0;
			if (seat->bindingRef != played) admission.NoteSeatSlot(status.stableSeat, played);
		}
	}

	NetH4SeatSimIdentity NetMatchService::WorldSimIdentityOfSeat(const NetWorldMembership& membership, uint16_t stableSeat) {
		const NetWorldSlot* bound = BoundWorldSlot(membership, stableSeat);
		if (bound == nullptr) {
			return {};
		}
		return {bound->peerId, static_cast<int32_t>(bound->team), true};
	}

	NetH4SeatSimIdentity NetMatchService::SeatSimIdentitySource(void* context, uint16_t stableSeat) {
		auto* service = static_cast<NetMatchService*>(context);
		if (service == nullptr) {
			return {};
		}
		return WorldSimIdentityOfSeat(service->m_WorldJoin.Membership(), stableSeat);
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
		if (!StreamsTail(session)) {
			return;
		}
		const uint8_t lobbyPeer = WorldJoinLobbyPeer(session);
		if (lobbyPeer == 0) {
			return;
		}
		// Whole frames with their round on the round's unreliable lane: a lost datagram delays only its own frames, and goes again.
		const NetPeerId connection = session.connection;
		const uint64_t nowMs = SteadyNowMs();
		host.NoteTailLinkRtt(connection, lobby.GetPeerPingMs(lobbyPeer));
		for (int datagram = 0; datagram < 32; ++datagram) {
			std::vector<uint8_t> packed;
			bool large = false;
			if (!host.NextTailDatagram(connection, nowMs, packed, &large)) {
				// A frame too large for a datagram goes on the ordered lane in pieces, one a pump; the datagrams go on after it.
				if (large && host.NextTailChunk(connection, packed) && lobby.SendPayloadTo(lobbyPeer, MakeWorldTailChunk(host.TailRound(), packed), nullptr))
					host.NoteTailChunkSent(connection, packed.size());
				return;
			}
			if (!lobby.SendPayloadTo(lobbyPeer, MakeWorldTailChunk(host.TailRound(), packed), nullptr, NetTransportLane::BulkUnreliable)) {
				host.NoteTailDatagramRefused(connection);
				return;
			}
		}
	}

	void NetMatchService::BoundPrivateImageWait(uint64_t nowMs) {
		std::vector<NetPeerId> waiting;
		for (const NetWorldJoinSession& session: m_WorldJoin.Sessions())
			if (session.phase == NetWorldJoinPhase::SnapshotTransfer && !session.transferStarted) waiting.push_back(session.connection);
		if (waiting.empty()) {
			m_PrivateImageRecaptured = false;
			return;
		}
		const bool stuck = m_PrivateImageTask.valid() && m_PrivateImageTakenMs != 0 && nowMs > m_PrivateImageTakenMs + c_PrivateImageWaitMs &&
		                   m_PrivateImageTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
		const bool retakeFailed = m_PrivateImageRecaptured && !m_PrivateImageRecapture && !m_PrivateImageTask.valid() && !m_PrivateJoinError.empty();
		if (!stuck && !retakeFailed) return;
		// A std::async future waits for its task when destroyed, so a stuck writer is waited out off the sim thread.
		if (stuck) std::thread([task = std::move(m_PrivateImageTask)]() mutable { task.wait(); }).detach();
		if (!m_PrivateImageRecaptured) {
			m_PrivateImageRecapture = m_PrivateImageRecaptured = true;
			m_PrivateJoinError.clear();
			std::ostringstream line;
			line << "[net-match] private checkpoint writer silent past " << c_PrivateImageWaitMs << "ms: taking a fresh one for " << waiting.size() << " returning seat(s)";
			System::PrintDiagnosticLine(line.str());
			return;
		}
		// The fresh capture never answered either: the returning seats stay with the AI that holds them.
		for (const NetPeerId connection: waiting) m_WorldJoin.CancelJoin(connection, "the host could not stage the rejoin image");
		m_PrivateImageRecaptured = false;
		std::ostringstream line;
		line << "[net-match] private rejoin refused: no checkpoint within two captures; " << waiting.size() << " seat(s) stay with the AI";
		System::PrintDiagnosticLine(line.str());
	}

	std::vector<uint8_t> NetMatchService::HeldSuccessionRoutes(const std::vector<uint8_t>& successorOrder, uint8_t lostHost, uint8_t localPeer,
	                                                          const std::vector<uint8_t>& reachable, const std::set<uint8_t>& held) {
		// A held seat is never a successor: the catch-up moves to a live one.
		std::vector<uint8_t> live;
		for (const uint8_t peer: reachable) if (!held.contains(peer)) live.push_back(peer);
		if (!live.empty()) return live;
		// Every survivor is held: the first of them in the match's order hosts, and the others dial it.
		for (const uint8_t peer: successorOrder) {
			if (peer == lostHost) continue;
			if (peer == localPeer) return {};
			if (std::find(reachable.begin(), reachable.end(), peer) != reachable.end()) return {peer};
		}
		return {};
	}

	namespace {
		/// A listener opened before the plane that owns it: what it heard first reaches that plane first, in order.
		class EarlyListener final : public INetTransport {
		public:
			EarlyListener(std::unique_ptr<INetTransport> inner, std::vector<NetTransportEvent> early) : m_Inner(std::move(inner)), m_Early(std::move(early)) {}
			bool StartHost(uint16_t port, std::string* error) override { return m_Inner->StartHost(port, error); }
			bool Connect(const std::string& address, uint16_t port, std::string* error) override { return m_Inner->Connect(address, port, error); }
			bool Send(NetPeerId peerId, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error, bool* congested) override {
				return m_Inner->Send(peerId, lane, bytes, error, congested);
			}
			void Disconnect(NetPeerId peerId, const std::string& reason) override { m_Inner->Disconnect(peerId, reason); }
			void Stop() override { m_Inner->Stop(); }
			std::vector<NetTransportEvent> PollEvents() override {
				std::vector<NetTransportEvent> events = std::move(m_Early);
				m_Early.clear();
				for (NetTransportEvent& event: m_Inner->PollEvents()) events.push_back(std::move(event));
				return events;
			}
			uint32_t GetPeerPingMs(NetPeerId peerId) const override { return m_Inner->GetPeerPingMs(peerId); }
			std::string GetConnectedRoute(NetPeerId peerId) const override { return m_Inner->GetConnectedRoute(peerId); }

		private:
			std::unique_ptr<INetTransport> m_Inner;
			std::vector<NetTransportEvent> m_Early;
		};
	} // namespace

	bool NetMatchService::HeldSeatListens(const std::vector<uint8_t>& successorOrder, uint8_t lostHost, uint8_t localPeer, const std::vector<uint8_t>& reachable,
	                                      const std::set<uint8_t>& held, const std::set<uint8_t>& departed) {
		// The first survivor in the match's order listens whatever it reads of the others: a held seat holding a later revision of the
		// lost host's holds may know it held and dial it.
		for (const uint8_t peer: successorOrder) {
			if (peer == lostHost || departed.contains(peer)) continue;
			if (peer == localPeer) return true;
			break;
		}
		return HeldSuccessionRoutes(successorOrder, lostHost, localPeer, reachable, held).empty();
	}

	bool NetMatchService::HeldSeatHostIsGone(bool linkLost, bool hasReject, NetRejectReason reason, bool ownStop, bool imageRejoin, uint64_t hostSilentMs, uint64_t hostRttMs) {
		// A dropped connection is carried as a plain disconnect; only this seat's own stop and the host's refusal say nothing of the host.
		if (linkLost) return !ownStop && !imageRejoin && (!hasReject || reason == NetRejectReason::SessionEnded || reason == NetRejectReason::Timeout || reason == NetRejectReason::InternalError || reason == NetRejectReason::HostLinkLost);
		return NetHostLinkLost(false, hostSilentMs, hostRttMs);
	}

	NetMatchService::LoneElection NetMatchService::LoneElectionOutcome(bool hostAnnounced, bool liveMembersUnheard) {
		// An announced leave is the host's decision; a lost host is absent, and the match goes on.
		if (hostAnnounced) return LoneElection::EndMatch;
		// Live members that went silent with the host say this peer lost its own link: it rejoins rather than host a match of its own.
		return liveMembersUnheard ? LoneElection::RejoinHost : LoneElection::HostAlone;
	}

	bool NetMatchService::PrivateReturnerInFlightLocked() const {
		return m_WorldJoin.IsPrivateMatch() && std::any_of(m_WorldJoin.Sessions().begin(), m_WorldJoin.Sessions().end(), [](const NetWorldJoinSession& session) {
			return !session.spectator && session.phase != NetWorldJoinPhase::Active && session.phase != NetWorldJoinPhase::Failed;
		});
	}

	void NetMatchService::DrivePrivateMatchRejoins(uint64_t nowMs) {
		if (!m_Runner || !m_Session || !m_Coordinator || !m_WorldJoin.IsPrivateMatch()) return;
		// A deferred repair starts once the returner is back, or past the headroom bound if it never gets there.
		if (m_HostRepairDeferred && (!PrivateReturnerInFlightLocked() || SteadyNowMs() - m_HostRepairDeferredMs > c_NetWorldHeadroomWaitMs)) {
			m_HostRepairDeferred = false;
			if (m_Coordinator->IsRunning() && !m_Coordinator->HasPendingRecoveryStop() && ResyncSnapshotAllowed(g_ActivityMan.GetActivity())) {
				System::PrintDiagnosticLine(std::string("[net-match] repair starts: ") + (PrivateReturnerInFlightLocked() ? "the returner did not finish its catch-up in time" : "the returning seat is back"));
				m_Coordinator->RequestResync("host requested repair");
				m_ResyncHealStartMs = SteadyNowMs();
			} else {
				m_HostRepairPending = false;
			}
		}
		m_WorldJoin.ExpireStaleJoins(nowMs);
		AnswerStalledReturnersLocked(nowMs);
		BoundPrivateImageWait(SteadyNowMs());
		const auto ready = m_Session->GetReadyPeers();
		std::vector<NetPeerId> live;
		uint64_t sentThrough = m_Coordinator->SentInputThrough();
		for (const auto& [peer, stats]: m_Coordinator->GetStats().peers) sentThrough = std::max(sentThrough, stats.highestTargetFrame);
		m_WorldJoin.NoteSentInputThrough(sentThrough);
		for (const auto& peer: ready) {
			live.push_back(peer.transportPeerId);
			if (m_Coordinator->UsesTransportPeer(peer.transportPeerId)) continue;
			const NetWorldJoinSession* open = m_WorldJoin.FindSession(peer.transportPeerId);
			if (open && open->phase != NetWorldJoinPhase::Active) continue;
			const uint8_t member = static_cast<uint8_t>(peer.assignedPeerId + 1);
			if (!m_Coordinator->HasHeldAISeat(member)) continue;
			// A returner told this round is over waits for the next round's start, which offers its seat again.
			if (m_ToldMatchOver.contains(peer.transportPeerId)) continue;
			const std::optional<uint16_t> seat = m_ReconnectHost.StableSeatOfConnection(peer.transportPeerId);
			NetPeerId holder = c_InvalidNetPeerId; uint32_t generation = 0, incarnation = 0;
			if (!seat || !m_ReconnectHost.GetSeatHolder(*seat, holder, generation, incarnation) || holder != peer.transportPeerId) continue;
			if (const auto bumps = m_InPlaceIncarnationBumps.find(member); bumps != m_InPlaceIncarnationBumps.end()) incarnation += bumps->second;
			// The held seat's own connection stays up: its player catches up in place on it, so its reports must reach the round.
			// A relaunched returner is a new incarnation and rejoins through the image.
			if (const auto holds = m_Coordinator->HeldTransactions(); holds.contains(member) && incarnation <= holds.at(member).seatIncarnation) {
				(void)m_Runner->GetLobbySession().BindWorldTransferRemote(member, holder, nullptr);
				continue;
			}
			if (open) continue;
			// A seat the round started held carries the ticket's incarnation in the round already, so its return must be the next one.
			if (const auto& known = m_Coordinator->GetConfig().peerIncarnations; known.contains(member)) {
				const uint32_t returning = ImageReturnIncarnation(incarnation, known.at(member));
				if (returning > incarnation) m_InPlaceIncarnationBumps[member] += returning - incarnation;
				incarnation = returning;
			}
			// A returner that kept its world reports its state before anything else; one that says nothing else first takes the image.
			m_Runner->GetLobbySession().BindWorldTransferRemote(member, holder, nullptr);
			// A return already agreed is on its way; if it passes, the seat is held again and its player answered then.
			if (!m_Runner->GetLobbySession().IsRemoteConnectionLobbyUp(member) || m_Coordinator->HasAgreedSeatReclaim(member)) continue;
			if (!RosterOffersReturnLocked(member, holder)) continue;
			std::string error;
			if (!m_WorldJoin.BeginRejoin(holder, *seat, member, incarnation, peer.displayName, nowMs, &error)) continue;
			{
				std::ostringstream line;
				line << "[net-match] private rejoin peer=" << static_cast<int>(member) << " incarnation=" << incarnation;
				System::PrintDiagnosticLine(line.str());
			}
		}
		m_WorldJoin.ReleaseLostConnections(live);
		std::erase_if(m_PrivateActivations, [&](NetPeerId connection) { return m_WorldJoin.FindSession(connection) == nullptr; });
		std::erase_if(m_PrivateTransferHeldReasons, [&](const auto& entry) { return m_WorldJoin.FindSession(entry.first) == nullptr; });
		std::erase_if(m_ReturnerLinkSettlesMs, [&](const auto& entry) { return m_WorldJoin.FindSession(entry.first) == nullptr; });
		std::erase_if(m_PrivateJoinBlobs, [&](const auto& task) { return m_WorldJoin.FindSession(task.first) == nullptr &&
		    task.second.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; });
		for (const auto& session: m_WorldJoin.Sessions()) {
			// The returner's lobby answers only once it is up: its seat, config and successor capsule go then, and before the image,
			// or it waits for a config that was sent while it could not hear it.
			if (session.phase == NetWorldJoinPhase::SnapshotTransfer && !session.matchConfigSent && m_Runner->GetLobbySession().IsRemoteConnectionLobbyUp(session.assignedPeerId) &&
			    m_Runner->GetLobbySession().SendMatchConfigTo(session.assignedPeerId)) {
				SendSuccessorCapsuleToLocked(session.assignedPeerId);
				m_WorldJoin.NoteMatchConfigSent(session.connection);
			}
			m_Coordinator->NoteReturningLink(session.assignedPeerId, session.connection, NetLockstepNowMs());
			// Our image to a returner fills its link, and the link's smoothed round trip takes a moment to come back once the image has
			// landed: a sample taken then times our own queue, never the link the seat will play on.
			if (session.phase == NetWorldJoinPhase::SnapshotTransfer && session.transferStarted) m_ReturnerLinkSettlesMs[session.connection] = nowMs + c_ReturnerLinkSettleMs;
			const auto settles = m_ReturnerLinkSettlesMs.find(session.connection);
			const bool linkCarriesOurImage = settles != m_ReturnerLinkSettlesMs.end() && nowMs < settles->second;
			m_WorldJoin.NoteRejoinLinkFit(session.connection, !linkCarriesOurImage && PrepareHeldPeerRejoinLocked(session.assignedPeerId));
			if (INetTransport* wire = ActiveWireLocked()) m_WorldJoin.NoteTailLinkRtt(session.connection, wire->GetPeerPingMs(session.connection));
			// A returning seat takes the base being captured for it, not the older one that capture replaces.
			if (session.phase == NetWorldJoinPhase::SnapshotTransfer && !session.transferStarted) {
				std::string why = !m_WorldJoin.Image().IsValid() ? "no base yet" : m_PrivateImageTask.valid() ? "its base is being written" :
				                  PrivateBaseWantedLocked(SteadyNowMs()) ? "a fresher base is wanted" : "";
				if (why.empty() && !StartJoinerImageTransfer(session, &why, nullptr) && why.empty()) why = "its lobby is not up";
				// A returner whose image does not leave says why, once per reason.
				if (!why.empty() && m_PrivateTransferHeldReasons[session.connection] != why) {
					m_PrivateTransferHeldReasons[session.connection] = why;
					System::PrintDiagnosticLine("[net-match] private transfer waits peer=" + std::to_string(session.assignedPeerId) + ": " + why);
				}
			}
			if (session.phase == NetWorldJoinPhase::CatchingUp) SendWorldJoinTailTo(m_Runner->GetLobbySession(), m_WorldJoin, session);
		}
		PumpWorldJoinLobby(nowMs);
		std::vector<NetPeerId> missed;
		for (const auto& session: m_WorldJoin.Sessions()) {
			if (session.activationTick == 0 || session.phase != NetWorldJoinPhase::CatchingUp) continue;
			if (!m_PrivateActivations.contains(session.connection)) {
				const uint64_t head = std::max(m_Coordinator->GetStats().nextFrame, m_Coordinator->SentInputThrough());
				uint64_t noticeTicks = 1;
				for (const auto& [peer, stats]: m_Coordinator->GetStats().peers) {
					if (peer == session.assignedPeerId || m_Coordinator->IsPeerGoneAtFrame(peer, head) || m_Coordinator->IsSeatUnderAI(peer, head)) continue;
					noticeTicks = std::max(noticeTicks, static_cast<uint64_t>(std::ceil((stats.pingMs + stats.jitterMs) / m_Coordinator->GetConfig().simTickMs)));
				}
				if (session.activationTick > head + noticeTicks + 1) continue;
				const uint64_t nowFrame = m_Coordinator->GetStats().nextFrame;
				const uint64_t behind = nowFrame > session.acknowledgedThrough ? nowFrame - session.acknowledgedThrough : 0;
				if (session.activationTick <= head || behind > session.activationTrailFrames + m_Coordinator->GetConfig().slowPlayerBoundTicks) {
					const uint64_t previous = session.activationTick;
					uint64_t later = 0;
					if (m_WorldJoin.ReannounceActivation(session.connection, nowFrame, &later, nullptr)) {
						m_Coordinator->MoveObservationEpoch(previous, later);
						(void)m_Runner->GetLobbySession().SendPayloadTo(WorldJoinLobbyPeer(session), MakeWorldJoinReport(c_NetWorldReportActivate, later), nullptr);
						System::PrintDiagnosticLine("[net-match] private activation waits for its tail peer=" + std::to_string(session.assignedPeerId) + " previous=" + std::to_string(previous) + " e=" + std::to_string(later));
					}
					continue;
				}
				// The returner reclaims only at the frame it was told, and a park re-stamps a reclaim it covers: an activation
				// a named or agreed park may reach is moved past the park and told again, never scheduled into it.
				const uint64_t simulated = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
				if (m_OpenCaptureTick != 0 && m_OpenCaptureTick >= simulated && m_OpenCaptureTick <= session.activationTick) continue;
				if (m_Coordinator->CaptureParkMayReach(session.activationTick)) {
					if (!MovePrivateActivationPastPark(session)) missed.push_back(session.connection);
					continue;
				}
				std::string error;
				if (!m_Coordinator->SchedulePeerReclaim(session.assignedPeerId, session.connection, session.incarnation, session.activationTick, &error, session.activationTrailFrames)) {
					if (m_PrivateTransferHeldReasons[session.connection] != error) {
						m_PrivateTransferHeldReasons[session.connection] = error;
						System::PrintDiagnosticLine("[net-match] reclaim not scheduled peer=" + std::to_string(session.assignedPeerId) + " e=" + std::to_string(session.activationTick) +
						                            " incarnation=" + std::to_string(session.incarnation) + ": " + error);
					}
					m_PrivateJoinError = error; continue;
				}
				m_PrivateActivations.insert(session.connection);
				m_WorldJoin.MarkActivationProposed(session.connection);
				(void)m_Coordinator->SendReturnerTheRoundFrom(session.assignedPeerId, session.activationTick);
				std::erase_if(m_PendingHeldReseats, [&](const auto& pending) { return pending.newOwnerPeerId == session.assignedPeerId; });
				std::erase_if(m_PendingHeldResolutions, [&](const auto& pending) { return pending.lockstepPeerId == session.assignedPeerId; });
			}
			if (session.acknowledgedThrough + 1 >= session.activationTick) {
				m_Coordinator->NoteReturnerCaughtUp(session.assignedPeerId, NetLockstepNowMs());
				m_WorldJoin.CompleteActivation(session.connection, session.activationTick, nullptr);
			}
			// A returner still replaying toward its reclaim frame is judged from its catch-up's end, not from the reclaim.
			else if (session.lastCatchUpReportMs != 0 && nowMs >= session.lastCatchUpReportMs && nowMs - session.lastCatchUpReportMs < c_ReturnerReportGapMs)
				m_Coordinator->NoteReturnerCatchingUp(session.assignedPeerId, NetLockstepNowMs());
		}
		// After the walk: CancelJoin erases from the vector the loop above is iterating.
		for (const NetPeerId connection: missed) m_WorldJoin.CancelJoin(connection, "the returner could not be moved past the capture park");
	}

	void NetMatchService::FeedRosterReturnsLocked() {
		for (const uint8_t peer: m_WorldJoin.TakeCancelledJoins()) m_ReconnectHost.NoteReturnAborted(peer);
		for (const NetWorldJoinSession& session: m_WorldJoin.Sessions())
			if (session.phase == NetWorldJoinPhase::CatchingUp || session.phase == NetWorldJoinPhase::Active) m_ReconnectHost.NoteReturnWorldReady(session.assignedPeerId);
		if (!m_Coordinator || !m_Coordinator->IsRunning()) return;
		const uint64_t next = m_Coordinator->GetStats().nextFrame;
		if (next == 0) return;
		// A returner is back when the round needs its seat's input at a committed frame and the AI no longer plays it.
		for (const uint8_t peer: m_ReconnectHost.ReturningPeers())
			if (m_Coordinator->SeatPlaysAtFrame(peer, next - 1)) m_ReconnectHost.NoteReturnCaughtUp(peer);
		// The round's own hold of a playing seat whose link stays open is the roster's too, and so is that seat's return in place.
		for (const uint8_t peer: m_ReconnectHost.PlayingPeers())
			if (m_Coordinator->HasHeldAISeat(peer) && m_Coordinator->IsSeatUnderAI(peer, next - 1))
				m_ReconnectHost.NoteSeatHeldInPlace(peer, m_Coordinator->IsHeldAsSlowMachine(peer) ? NetSeatHoldCause::Capacity : NetSeatHoldCause::LateStream);
		for (const uint8_t peer: m_ReconnectHost.HeldInPlacePeers())
			if (m_Coordinator->SeatPlaysAtFrame(peer, next - 1)) m_ReconnectHost.NoteSeatPlaysAgain(peer);
	}

	bool NetMatchService::RosterOffersReturnLocked(uint8_t lockstepPeerId, NetPeerId holder) {
		const NetRosterSeat* seat = m_ReconnectHost.RosterSeatOfPeer(lockstepPeerId);
		if (!seat || seat->phase != NetSeatPhase::Held || seat->holdCause != NetSeatHoldCause::RejoinFailed) return true;
		std::string refusal;
		if (m_ReconnectHost.ReofferReturn(lockstepPeerId, &refusal)) return true;
		if (!refusal.empty()) {
			System::PrintDiagnosticLine("[net-match] return refused peer=" + std::to_string(static_cast<int>(lockstepPeerId)) + ": " + refusal);
			RefuseReturnerLocked(holder, refusal, refusal);
		}
		return false;
	}

	void NetMatchService::SendSuccessorCapsuleToLocked(uint8_t member) {
		// A match that names its successors withholds its config from a returner until it holds its successor capsule,
		// so the capsule follows the config as a lobby round sends them.
		NetLobbySession& lobby = m_Runner->GetLobbySession();
		if (lobby.GetMatchConfig().successorOrder.empty()) return;
		NetLobbyMigration capsule;
		capsule.kind = 2;
		capsule.peerId = member;
		capsule.configHash = lobby.GetMatchConfigHash();
		if (!SealMigrationCapsuleLocked(member, capsule.configHash, capsule.sealedState)) {
			System::PrintDiagnosticLine("[net-match] no successor capsule could be sealed for returning peer " + std::to_string(member));
			return;
		}
		(void)lobby.SendPayloadTo(member, capsule, nullptr);
	}

	void NetMatchService::AnswerStalledReturnersLocked(uint64_t nowMs) {
		// A returner that replays slower than the round plays keeps catching up: its activation waits until its replay shows
		// headroom, so no peer ever waits on it, until the round's history sends it back for a fresh image past the lag limit.
		for (const NetPeerId connection: m_WorldJoin.ReturnersWithoutHeadroom(nowMs, c_NetWorldHeadroomWaitMs)) {
			const NetWorldJoinSession* session = m_WorldJoin.FindSession(connection);
			if (!session || !m_SlowReturnersNoted.insert(connection).second) continue;
			const uint64_t horizon = m_Coordinator ? m_Coordinator->GetStats().nextFrame : 0;
			std::ostringstream line;
			line << "[net-match] returner peer=" << static_cast<int>(session->assignedPeerId) << " stands " << (horizon > session->acknowledgedThrough ? horizon - session->acknowledgedThrough : 0)
			     << " frames behind the round after " << c_NetWorldHeadroomWaitMs << "ms: it keeps catching up and activates inside the lead";
			System::PrintDiagnosticLine(line.str());
		}
		std::erase_if(m_SlowReturnersNoted, [&](NetPeerId connection) { return m_WorldJoin.FindSession(connection) == nullptr; });
		// A match its own rules decided has no base left to take: a returner still waiting for one is told the match is over, at once.
		if (m_WorldJoin.IsPrivateMatch() && m_Session && ClassifyRejoin(g_ActivityMan.GetActivity()) == NetRejoinAnswer::MatchOver) {
			std::vector<NetPeerId> ended;
			for (const NetWorldJoinSession& session: m_WorldJoin.Sessions())
				if (!session.spectator && session.phase == NetWorldJoinPhase::SnapshotTransfer && !session.transferStarted) ended.push_back(session.connection);
			for (const NetPeerId connection: ended) {
				System::PrintDiagnosticLine("[net-match] rejoin answered connection=" + std::to_string(connection) + ": the match is over");
				m_WorldJoin.CancelJoin(connection, "the match is over");
				m_ToldMatchOver.insert(connection);
			}
			if (!ended.empty() && m_Coordinator) {
				const uint64_t resume = m_Coordinator->GetResumeFrame();
				AnswerEndedReturnersLocked(resume > 0 ? resume - 1 : 0);
			}
		}
		// A returner whose base never comes (the round cannot capture one) is told so, never left waiting on it.
		std::vector<NetPeerId> unserved;
		for (const NetWorldJoinSession& session: m_WorldJoin.Sessions()) {
			if (session.spectator || session.phase != NetWorldJoinPhase::SnapshotTransfer || session.transferStarted || m_PrivateImageTask.valid() || session.openedAtMs == 0) continue;
			if (m_WorldJoin.IsPrivateMatch() && nowMs > session.openedAtMs && nowMs - session.openedAtMs > 2 * c_PrivateImageWaitMs) unserved.push_back(session.connection);
		}
		for (const NetPeerId connection: unserved) {
			System::PrintDiagnosticLine("[net-match] rejoin refused connection=" + std::to_string(connection) + ": no image could be staged for it in " + std::to_string(2 * c_PrivateImageWaitMs) + "ms" +
			                            (m_PrivateBaseHeldReason ? std::string(" (") + m_PrivateBaseHeldReason + ")" : std::string()));
			RefuseReturnerLocked(connection, "no image could be staged for the returner", "The host could not stage your rejoin; rejoining again");
		}
	}

	void NetMatchService::RefuseReturnerLocked(NetPeerId connection, const std::string& reason, const std::string& text) {
		if (m_Session) m_Session->DisconnectReadyPeer(connection, NetRejectReason::HostNotAccepting, text);
		m_WorldJoin.CancelJoin(connection, reason);
	}

	bool NetMatchService::MovePrivateActivationPastPark(const NetWorldJoinSession& session) {
		const uint64_t previous = session.activationTick;
		uint64_t clear = previous;
		// A park is bounded by its capture and its answer budget, so the first frame it cannot reach is close.
		while (m_Coordinator->CaptureParkMayReach(clear) && clear < previous + 36000) ++clear;
		const NetPeerId connection = session.connection;
		const uint8_t lobbyPeer = WorldJoinLobbyPeer(session);
		const uint8_t member = session.assignedPeerId;
		uint64_t later = 0;
		if (!m_WorldJoin.ReannounceActivation(connection, clear, &later, nullptr)) {
			System::PrintDiagnosticLine("[net-match] private activation missed peer=" + std::to_string(member) + " e=" + std::to_string(previous) + ": a capture park covers it and the returner was already moved once");
			return false;
		}
		if (m_Runner) (void)m_Runner->GetLobbySession().SendPayloadTo(lobbyPeer, MakeWorldJoinReport(c_NetWorldReportActivate, later), nullptr);
		System::PrintDiagnosticLine("[net-match] private activation moved past the capture park peer=" + std::to_string(member) + " e=" + std::to_string(later) + " previous=" + std::to_string(previous));
		return true;
	}

	void NetMatchService::NoteAnnouncedActivationsLocked() {
		uint64_t earliest = 0;
		if (m_IsHost)
			for (const auto& session: m_WorldJoin.Sessions())
				if (session.phase == NetWorldJoinPhase::CatchingUp && session.activationTick != 0 && !session.activationProposed && (earliest == 0 || session.activationTick < earliest))
					earliest = session.activationTick;
		m_AnnouncedActivationTick.store(earliest, std::memory_order_relaxed);
	}

	void NetMatchService::DriveWorldJoins(uint64_t nowMs) {
		NoteAnnouncedActivationsLocked();
		if (m_WorldJoin.IsPrivateMatch()) { DrivePrivateMatchRejoins(nowMs); return; }
		if (!m_WorldJoin.IsConfigured() || !m_Coordinator || !m_Session) {
			return;
		}
		m_WorldJoin.ExpireStaleJoins(nowMs);
		AnswerStalledReturnersLocked(nowMs);
		// The opening checkpoint is the round's state only at its anchor; past it a returning seat takes the newest image.
		if (m_Runner && m_Coordinator->IsRunning()) {
			NetLobbySession& lobby = m_Runner->GetLobbySession();
			if (const uint64_t anchor = lobby.ResumeOfferTick(); anchor != 0 && m_Coordinator->GetStats().nextFrame > anchor + 1) {
				lobby.RetireResumeOffer();
				System::PrintDiagnosticLine("[net-world] opening resume offer retired at frame " + std::to_string(m_Coordinator->GetStats().nextFrame) + " past anchor " + std::to_string(anchor));
			}
		}
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
		// A dropped or reclaiming seat keeps its slot: only that holder may take it back, and a
		// fresh join that arrives meanwhile watches instead of allocating it.
		std::set<uint8_t> aiHeld;
		for (const NetWorldSlot& slot: m_WorldJoin.Membership().Slots()) if (m_Coordinator->HasHeldAISeat(slot.peerId)) aiHeld.insert(slot.peerId);
		// A held member whose player left chose to go: its seat is released (the AI keeps its units) and its slot opens for a
		// new join, never waiting for a reclaim that cannot come.
		for (size_t leaves = m_WorldJoin.Membership().Slots().size(); leaves > 0; --leaves) {
			WorldCleanLeave held;
			if (!FindHeldWorldCleanLeave(m_ReconnectHost.GetSeatStatuses(), m_WorldJoin, aiHeld, held)) break;
			aiHeld.erase(held.peerId);
			m_Coordinator->ResolveHeldSeat(held.peerId, NetLockstepHoldResolution::Expired, nowMs);
			NetGameWorldTransition release;
			release.kind = NetGameWorldTransition::Release;
			release.peerId = held.peerId;
			release.holderGeneration = held.holderGeneration;
			release.team = held.team;
			(void)m_WorldJoin.Membership().Release(held.peerId, nullptr);
			(void)ScenarioRunner::SubmitWorldTransition(release);
			NoteWorldReleaseLocked(m_ReconnectHost.GetSeatStatuses(), held.peerId, held.stableSeat, m_Coordinator->GetStats().nextFrame);
			if (held.connection != c_InvalidNetPeerId) m_WorldJoin.CancelJoin(held.connection, "left while the AI held the seat");
			System::PrintDiagnosticLine("[net-world] release held peer=" + std::to_string(static_cast<int>(held.peerId)) + ": its player left while the AI held the seat");
		}
		// A member the roster seated before the round started holds its slot: a later join never takes it.
		(void)BindSeatedWorldMembers(m_ReconnectHost.GetSeatStatuses(), m_ReconnectHost.GetRoster(), m_WorldJoin.Membership());
		m_WorldJoin.NoteReclaimHolds(WorldReclaimHoldSlots(m_ReconnectHost.GetSeatStatuses(), m_WorldJoin.Membership(), aiHeld));
		m_WorldSpectatorsFree = static_cast<int64_t>(m_WorldJoin.SpectatorsFree());
		bool answeredRefusal = false;
		for (const NetSessionPeerInfo& peer: readyPeers) {
			if (m_Coordinator->UsesTransportPeer(peer.transportPeerId)) {
				continue;
			}
			if (m_WorldJoin.FindSession(peer.transportPeerId) != nullptr) {
				continue;
			}
			const std::optional<uint16_t> seated = m_ReconnectHost.StableSeatOfConnection(peer.transportPeerId);
			if (!seated) {
				continue;
			}
			const uint16_t stableSeat = *seated;
			if (m_WorldJoin.RefusalOf(peer.transportPeerId) != NetWorldJoinRefusal::None) {
				continue;
			}
			std::string joinError;
			NetPeerId holderConnection = c_InvalidNetPeerId;
			uint32_t holderGeneration = 0;
			uint32_t holderIncarnation = 0;
			const bool credentialedHolder = m_ReconnectHost.GetSeatHolder(stableSeat, holderConnection, holderGeneration, holderIncarnation) &&
			                                holderConnection == peer.transportPeerId;
			if (credentialedHolder) {
				uint8_t played = 0;
				for (const NetH4Seat& seat: m_ReconnectHost.GetSeatTable())
					if (seat.stableSeat == stableSeat) played = m_ReconnectHost.SimIdentityOfSeat(seat).peerId;
				if (played != 0 && !RosterOffersReturnLocked(played, peer.transportPeerId)) continue;
			}
			uint8_t seatPeerId = 0;
			for (const NetH4Seat& seat: m_ReconnectHost.GetSeatTable())
				if (seat.stableSeat == stableSeat) seatPeerId = seat.lockstepPeerId;
			if (!m_WorldJoin.BeginJoin(peer.transportPeerId, stableSeat, peer.displayName, nowMs, &joinError, credentialedHolder, seatPeerId)) {
				// A world with no seat and no watcher slot answers the connection once, before it has
				// read a byte of image, with the reason the joiner shows.
				const NetWorldJoinRefusal refusal =
				    joinError == NetWorldJoinRefusalText(static_cast<uint64_t>(NetWorldJoinRefusal::WorldFull)) ? NetWorldJoinRefusal::WorldFull
				    : joinError == NetWorldJoinRefusalText(static_cast<uint64_t>(NetWorldJoinRefusal::SeatHeld)) ? NetWorldJoinRefusal::SeatHeld
				                                                                                                 : NetWorldJoinRefusal::None;
				// One refusal per pump: the reserved id carries one answer at a time, and a connection
				// left unanswered is asked again next pump, where a freed seat may admit it instead.
				if (refusal != NetWorldJoinRefusal::None && !answeredRefusal && m_Runner &&
				    AnswerWorldJoinRefusal(m_Runner->GetLobbySession(), peer.transportPeerId, refusal)) {
					(void)m_WorldJoin.NoteRefusal(peer.transportPeerId, refusal);
					answeredRefusal = true;
					{
						std::ostringstream line;
						line << "[net-world] refuse connection=" << peer.transportPeerId << " " << joinError;
						System::PrintDiagnosticLine(line.str());
					}
				}
				continue;
			}
			// The session pump may run inside a tick; capture after the world finishes it.
			NoteWorldJoinWantsCapture();
			if (const NetWorldJoinSession* session = m_WorldJoin.FindSession(peer.transportPeerId)) {
				if (m_Runner) {
					const uint8_t lobbyPeer = WorldJoinLobbyPeer(*session);
					if (lobbyPeer != 0) {
						(void)m_Runner->GetLobbySession().BindWorldTransferRemote(lobbyPeer, peer.transportPeerId, nullptr);
					}
				}
			}
			{
				std::ostringstream line;
				line << "[net-world] join connection=" << peer.transportPeerId << " name=" << peer.displayName;
				System::PrintDiagnosticLine(line.str());
			}
		}
		std::vector<std::pair<NetPeerId, std::string>> unstartable;
		for (const NetWorldJoinSession& session: m_WorldJoin.Sessions()) {
			if (session.phase == NetWorldJoinPhase::SnapshotTransfer && !session.transferStarted && m_WorldJoin.Image().IsValid()) {
				std::string transferError;
				bool cannotStart = false;
				if (!StartJoinerImageTransfer(session, &transferError, &cannotStart)) {
					if (!transferError.empty()) {
						std::ostringstream line;
						line << "[net-world] transfer wait connection=" << session.connection << " " << transferError;
						System::PrintDiagnosticLine(line.str());
					}
					if (cannotStart) {
						unstartable.emplace_back(session.connection, transferError);
					}
				}
			}
			if (StreamsTail(session) && m_Runner) {
				SendWorldJoinTailTo(m_Runner->GetLobbySession(), m_WorldJoin, session);
			}
		}
		// After the walk: CancelJoin erases from the vector the loop above is iterating.
		for (const auto& [connection, reason]: unstartable) {
			m_WorldJoin.CancelJoin(connection, reason);
			{
				std::ostringstream line;
				line << "[net-world] cancel bootstrap connection=" << connection << " " << reason;
				System::PrintDiagnosticLine(line.str());
			}
		}
		if (m_Runner) {
			m_Runner->GetLobbySession().PumpOutgoingChunks();
		}
		const uint64_t nowFrame = m_Coordinator->GetStats().nextFrame;
		while (const NetWorldJoinSession* slow = m_WorldJoin.SlowActivation(g_TimerMan.GetSimUpdateCount())) {
			uint64_t later = 0;
			const uint64_t previous = slow->activationTick;
			if (m_WorldJoin.ReannounceActivation(slow->connection, nowFrame, &later, nullptr)) {
				// A re-announce moves this activation's restart; every other joiner's stays announced.
				m_Coordinator->MoveObservationEpoch(previous, later);
				if (m_Runner) {
					(void)m_Runner->GetLobbySession().SendPayloadTo(WorldJoinLobbyPeer(*slow), MakeWorldJoinReport(c_NetWorldReportActivate, later), nullptr);
				}
				{
					std::ostringstream line;
					line << "[net-world] reannounce peer=" << static_cast<int>(slow->assignedPeerId) << " e=" << later
				          << " previous=" << previous << " applied=" << slow->acknowledgedThrough << " delivered=" << slow->deliveredThrough
				          << " input_horizon=" << nowFrame << " simulated=" << g_TimerMan.GetSimUpdateCount();
					System::PrintDiagnosticLine(line.str());
				}
				continue;
			}
			// CancelJoin erases the session this pointer names, so the id is read before the call.
			const NetPeerId cancelled = slow->connection;
			{
				std::ostringstream line;
				line << "[net-world] activation missed connection=" << cancelled << " e=" << slow->activationTick
			          << " applied=" << slow->acknowledgedThrough << " delivered=" << slow->deliveredThrough
			          << " input_horizon=" << nowFrame << " simulated=" << g_TimerMan.GetSimUpdateCount();
				System::PrintDiagnosticLine(line.str());
			}
			m_WorldJoin.CancelJoin(cancelled, "the joiner missed the announced activation");
			{
				std::ostringstream line;
				line << "[net-world] cancel slow join connection=" << cancelled;
				System::PrintDiagnosticLine(line.str());
			}
		}
		const std::vector<NetH4SeatStatus> seatStatuses = m_ReconnectHost.GetSeatStatuses();
		// One clean leave per slot at most: each Release frees the slot it names, so the walk ends.
		for (size_t leaves = m_WorldJoin.Membership().Slots().size(); leaves > 0; --leaves) {
			WorldCleanLeave leave;
			if (!FindWorldCleanLeave(seatStatuses, m_WorldJoin, leave)) {
				break;
			}
			NetGameWorldTransition release;
			release.kind = NetGameWorldTransition::Release;
			release.peerId = leave.peerId;
			release.holderGeneration = leave.holderGeneration;
			release.team = leave.team;
			(void)m_WorldJoin.Membership().Release(leave.peerId, nullptr);
			(void)ScenarioRunner::SubmitWorldTransition(release);
			if (leave.connection != c_InvalidNetPeerId) m_WorldJoin.CancelJoin(leave.connection, "clean leave");
			NoteWorldReleaseLocked(seatStatuses, leave.peerId, leave.stableSeat, nowFrame);
			{
				std::ostringstream line;
				line << "[net-world] release peer=" << static_cast<int>(leave.peerId);
				System::PrintDiagnosticLine(line.str());
			}
		}
		// A slot the host freed - however its member went - goes to the watcher waiting longest.
		if (m_WorldJoin.Membership().FreeSlots() > 0) {
			uint64_t promotedAt = 0;
			NetPeerId promoted = c_InvalidNetPeerId;
			if (PromoteWorldWatcher(m_WorldJoin, m_ReconnectHost, nowFrame, &promotedAt, &promoted) && promotedAt != 0) {
				// The promoted watcher takes the plan a fresh join takes: its own announced E, then one
				// Activate with one brain. Its member lobby id replaces the watcher id it gave back.
				m_Coordinator->SetObservationEpoch(promotedAt);
				if (const NetWorldJoinSession* session = m_WorldJoin.FindSession(promoted); session != nullptr && m_Runner) {
					NetLobbySession& lobby = m_Runner->GetLobbySession();
					// A promoted watcher is a world bootstrap again until its Activate commits, so its id is
					// bound on the world plane: a session-roster rebuild would drop a plain late binding and
					// stall the tail it still needs to reach E.
					(void)lobby.BindWorldTransferRemote(session->assignedPeerId, promoted, nullptr);
					lobby.SendMatchConfigTo(session->assignedPeerId);
					(void)lobby.SendPayloadTo(session->assignedPeerId, MakeWorldJoinReport(c_NetWorldReportActivate, promotedAt), nullptr);
				}
				{
					std::ostringstream line;
					line << "[net-world] promote connection=" << promoted << " at=" << promotedAt;
					System::PrintDiagnosticLine(line.str());
				}
				NoteWorldPromotionLocked(promoted, promotedAt);
			}
		}
		if (!m_WorldJoin.IsPrivateMatch()) PublishWorldSeatSlots(m_ReconnectHost, m_WorldJoin.Membership());
		DriveWorldSeatRespawns(nowFrame);
		for (const NetWorldJoinSession& session: m_WorldJoin.Sessions()) {
			if (session.phase != NetWorldJoinPhase::CatchingUp || session.activationTick == 0) continue;
			if (!session.activationProposed && session.acknowledgedActivation == session.activationTick && session.acknowledgedThrough + 6 >= g_TimerMan.GetSimUpdateCount()) {
				NetPeerId holder = c_InvalidNetPeerId; uint32_t generation = 0, incarnation = 0;
				if (!m_ReconnectHost.GetSeatHolder(session.stableSeat, holder, generation, incarnation) || holder != session.connection) continue;
				// A park commits empty frames, so no activation is agreed where a named or open park may reach it.
				const uint64_t simulated = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
				if ((m_OpenCaptureTick != 0 && m_OpenCaptureTick >= simulated) || m_Coordinator->CaptureParkMayReach(session.activationTick)) continue;
				std::string error;
				// A seat the AI holds is still this member's: it comes back by reclaiming the seat, never as a new member.
				if (m_Coordinator->HasHeldAISeat(session.assignedPeerId)) {
					if (m_Coordinator->SchedulePeerReclaim(session.assignedPeerId, session.connection, incarnation, session.activationTick, &error)) {
						m_WorldJoin.MarkActivationProposed(session.connection);
						m_HeldWorldReclaims.insert(session.connection);
					} else {
						System::PrintDiagnosticLine("[net-world] held seat reclaim refused peer=" + std::to_string(session.assignedPeerId) + " at=" + std::to_string(session.activationTick) + ": " + error);
					}
				} else {
					const auto transition = BuildWorldActivateTransition(session, m_Runner->GetMatchConfig(), m_WorldJoin.Membership().Revision());
					if (m_Coordinator->ProposeWorldAdmission(session.connection, incarnation, transition, &error)) m_WorldJoin.MarkActivationProposed(session.connection);
				}
			}
			if (session.activationProposed && !session.activationCommitted &&
			    (m_HeldWorldReclaims.contains(session.connection) || m_Coordinator->HasWorldAdmission(session.assignedPeerId, session.activationTick))) {
				m_WorldJoin.MarkActivationCommitted(session.connection);
				m_Runner->GetLobbySession().SendPayloadTo(WorldJoinLobbyPeer(session), MakeWorldJoinReport(c_NetWorldReportActivationCommit, session.activationTick), nullptr);
				{
					std::ostringstream line;
					line << "[net-world] activation agreed peer=" << static_cast<int>(session.assignedPeerId) << " at=" << session.activationTick
					     << (m_HeldWorldReclaims.contains(session.connection) ? " reclaim" : " admission");
					System::PrintDiagnosticLine(line.str());
				}
			}
			if (session.activationCommitted && session.acknowledgedThrough + 1 >= session.activationTick && g_TimerMan.GetSimUpdateCount() >= session.activationTick) {
				m_WorldJoin.CompleteActivation(session.connection, session.activationTick, nullptr);
				{
					std::ostringstream line;
					line << "[net-world] activate peer=" << static_cast<int>(session.assignedPeerId) << " at=" << session.activationTick;
					System::PrintDiagnosticLine(line.str());
				}
			}
		}
	}

	bool NetMatchService::SetWorldSpectatorDeclinesPromotion(bool declines) {
		// A watcher's own choice, sent on its world lobby; the host records it against its seat.
		if (m_IsHost || !m_Runner || !m_WorldCatchUp.active) {
			return false;
		}
		m_WorldSpectatorDeclinesPromotion = declines;
		return m_Runner->GetLobbySession().SendPayload(MakeWorldJoinReport(c_NetWorldReportDecline, declines ? 1 : 0), nullptr);
	}

	bool NetMatchService::AnswerWorldJoinRefusal(NetLobbySession& lobby, NetPeerId connection, NetWorldJoinRefusal refusal) {
		if (connection == c_InvalidNetPeerId || refusal == NetWorldJoinRefusal::None) {
			return false;
		}
		// The reserved id is scratch: nothing streams to it, and the answer goes out on this call,
		// before any later refusal can point it somewhere else.
		if (!lobby.BindLateRemote(c_WorldRefusalLobbyPeer, connection, nullptr)) {
			return false;
		}
		return lobby.SendPayloadTo(c_WorldRefusalLobbyPeer, MakeWorldJoinReport(c_NetWorldReportRefused, static_cast<uint64_t>(refusal)), nullptr);
	}

	void NetMatchService::DriveWorldSeatRespawns(uint64_t nowFrame) {
		const Activity* activity = g_ActivityMan.GetActivity();
		if (activity == nullptr || nowFrame == 0) {
			return;
		}
		const NetMatchConfig& config = m_Runner ? m_Runner->GetMatchConfig() : m_MatchConfig;
		// A seat with no living brain keeps its seat and watches; the host authors one respawn for it
		// after the world's configured delay, and every peer spawns it at the transition's frame.
		for (const NetWorldSlot& slot: m_WorldJoin.Membership().Slots()) {
			if (!slot.held) {
				continue;
			}
			const int32_t player = WorldActivityPlayerOf(config, slot.peerId);
			if (player < 0) {
				continue;
			}
			const Actor* brain = activity->GetPlayerBrain(player);
			(void)m_WorldJoin.Membership().NoteSeatBrain(slot.peerId, brain != nullptr && !brain->IsDead(), nowFrame);
		}
		const uint64_t delayFrames = WorldRespawnDelayFrames(config);
		const NetWorldSlot* dueRespawn = m_WorldJoin.Membership().DueSeatRespawn(nowFrame, delayFrames);
		if (dueRespawn == nullptr) {
			return;
		}
		const uint8_t peerId = dueRespawn->peerId;
		const NetGameWorldTransition respawn = BuildWorldSeatRespawnTransition(*dueRespawn, config, m_WorldJoin.Membership().Revision(), nowFrame);
		if (ScenarioRunner::SubmitWorldTransition(respawn)) {
			(void)m_WorldJoin.Membership().NoteSeatRespawn(peerId, nowFrame);
			{
				std::ostringstream line;
				line << "[net-world] seat respawn peer=" << static_cast<int>(peerId) << " at=" << nowFrame;
				System::PrintDiagnosticLine(line.str());
			}
		}
	}

	// The catch-up is armed on the worker thread, which owns the session until it hands it back, and the
	// replay that follows may never reach the service pump: the park is declared here, on that session.
	// The round this session just left never evaluated its timeouts, and the game thread that hands it
	// over then waits without pumping anything: the worker's first evaluation would measure the whole
	// match as silence. The windows start again at the handover instead.
	void NetMatchService::NoteSessionHandedToWorker(NetSession& session) {
		session.NotePumpParked();
	}

	void NetMatchService::NoteWorldCatchUpArmed(NetSession& session) {
		session.NotePumpParked();
		session.SetSilenceSuspended(true);
	}

	bool NetMatchService::PrepareReceivedWorldJoin(const std::vector<uint8_t>& bytes, const NetMatchConfig& adopted, std::string& pendingLoad, std::string* error) {
		NetWorldCheckpointImage image;
		std::vector<uint8_t> archive;
		std::vector<std::vector<uint8_t>> tailBytes;
		if (!DecodeWorldJoinImageBlob(bytes, image, archive, tailBytes, error)) {
			return false;
		}
		const auto name = "p5join_recv_" + std::to_string(System::GetProcessID());
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
		m_WorldCatchUp.privateMatch = image.privateSessionId != 0;
		// A world image carries no roster of its own; the restore maps this machine's seats from the adopted one.
		if (!m_WorldCatchUp.privateMatch) {
			m_WorldCatchUp.checkpointConfig = adopted;
			// Its lockstep state at the image's tick, when its host named one.
			m_WorldCatchUp.sideState = image.sideState;
		}
		m_WorldCatchUp.roundId = image.round;
		m_WorldCatchUp.authorityGeneration = image.authorityGeneration;
		m_WorldCatchUp.authorityPeerId = image.authorityPeerId;
		if (!m_WorldCatchUp.privateMatch && !AdoptWorldImageSeats(image, adopted.peerCount, m_WorldCatchUp, error)) return false;
		if (m_WorldCatchUp.privateMatch) {
			m_WorldCatchUp.initialPeerLeaves = image.departedPeers;
			std::vector<uint8_t> hash, holdBytes;
			NetLockstepFrame holds;
			if (image.privateSessionId != adopted.sessionId || !DecodeConfigPayload(image.checkpointConfig, m_WorldCatchUp.checkpointConfig) ||
			    m_WorldCatchUp.checkpointConfig.sessionId != adopted.sessionId || m_WorldCatchUp.checkpointConfig.activityPreset != adopted.activityPreset ||
			    NetIdentity::HashHex(NetMatchConfigUtil::HashConfig(m_WorldCatchUp.checkpointConfig)) != image.matchConfigHash ||
			    !ResumeBytes(image.roundConfigHash, hash) || hash.size() != m_WorldCatchUp.roundConfigHash.size() ||
			    !ResumeBytes(image.heldState, holdBytes) || !DecodeCommittedJoinFrame(holdBytes, holds, error) || holds.targetFrame != image.tick || holds.roundId != image.round) {
				if (error && error->empty()) *error = "private checkpoint identity differs from this match";
				return false;
			}
			if (image.authorityPeerId == 0 || image.authorityPeerId > adopted.peerCount || image.departedPeers.size() > adopted.peerCount) return false;
			for (const auto& [peer, frame]: image.departedPeers) if (peer == 0 || peer > adopted.peerCount || frame > image.tick) return false;
			std::copy(hash.begin(), hash.end(), m_WorldCatchUp.roundConfigHash.begin());
			m_WorldCatchUp.sideState = image.sideState;
			m_WorldCatchUp.pauseState = image.pauseState;
			for (const auto& command: holds.commands) {
				const auto* hold = std::get_if<NetGameSeatHold>(&command.payload);
				if (!hold || hold->peerId == 0 || hold->peerId > adopted.peerCount || hold->cutoffFrame > image.tick) return false;
				m_WorldCatchUp.initialHolds[hold->peerId] = *hold;
			}
		}
		m_WorldCatchUp.snapshotTick = image.tick;
		m_WorldCatchUp.appliedThrough = image.tick;
		m_WorldCatchUp.digest = image.digest;
		for (const std::vector<uint8_t>& encoded: tailBytes) {
			NetLockstepFrame frame;
			if (!DecodeCommittedJoinFrame(encoded, frame, error)) {
				return false;
			}
			if (m_WorldCatchUp.privateMatch && frame.roundId != image.round) {
				if (error) *error = "private tail belongs to another round: frame " + std::to_string(frame.targetFrame) + " is of round " + std::to_string(frame.roundId) + ", the image's is " + std::to_string(image.round);
				return false;
			}
			m_WorldCatchUp.tail.push_back(std::move(frame));
		}
		pendingLoad = name;
		return true;
	}

	void NetMatchService::StepWorldJoinCatchUpClient(NetLobbySession& lobby, NetWorldCatchUpClient& catchUp, uint64_t* outRefusal, std::optional<uint64_t>* outRoundEnded) {
		if (outRefusal) *outRefusal = 0;
		std::vector<std::pair<uint64_t, size_t>> foreign;
		// A private catch-up replays one round: a chunk of any other round is dropped, never replayed.
		const std::optional<uint64_t> ownRound = catchUp.privateMatch ? std::optional<uint64_t>(catchUp.roundId) : std::nullopt;
		const std::vector<std::vector<uint8_t>> datagrams = lobby.TakePendingTailDatagrams(ownRound, &foreign);
		std::vector<uint8_t> incoming = lobby.TakePendingTailBytes(ownRound, &foreign);
		for (const auto& [round, bytes]: foreign) {
			if (!catchUp.droppedForeignRounds.insert(round).second) continue;
			System::PrintDiagnosticLine("[net-match] dropped a tail chunk of round " + std::to_string(round) + " (" + std::to_string(bytes) + " bytes): this catch-up replays round " + std::to_string(catchUp.roundId));
		}
		if (incoming.size() + catchUp.partialTail.size() > 32ULL * 1024 * 1024) {
			ScenarioRunner::SetControllerReplayError("private tail exceeds its bounded receive buffer"); return;
		}
		catchUp.partialTail.insert(catchUp.partialTail.end(), incoming.begin(), incoming.end());
		std::vector<NetLockstepFrame> later;
		// Reads whole records from packed at offset; a stream may end inside one, a datagram never does.
		const auto readRecords = [&](const std::vector<uint8_t>& packed, size_t& offset) {
			while (offset + 4 <= packed.size()) {
				const uint32_t size = static_cast<uint32_t>(packed[offset]) | (static_cast<uint32_t>(packed[offset + 1]) << 8) |
				                      (static_cast<uint32_t>(packed[offset + 2]) << 16) | (static_cast<uint32_t>(packed[offset + 3]) << 24);
				if (size == 0 || size > 2 * NetLockstepCodec::c_MaxPeerCount * NetLockstepCodec::c_MaxRecoveryInputBytes) {
					ScenarioRunner::SetControllerReplayError("invalid committed tail record length"); return false;
				}
				if (packed.size() - offset - 4 < size) break;
				offset += 4;
				NetLockstepFrame frame;
				std::string error;
				if (!DecodeCommittedJoinFrame(std::vector<uint8_t>(packed.begin() + offset, packed.begin() + offset + size), frame, &error)) {
					ScenarioRunner::SetControllerReplayError("invalid committed tail: " + error); return false;
				}
				if (catchUp.privateMatch && frame.roundId != catchUp.roundId) {
					ScenarioRunner::SetControllerReplayError("private tail belongs to another round: frame " + std::to_string(frame.targetFrame) + " is of round " + std::to_string(frame.roundId) + ", the catch-up's is " + std::to_string(catchUp.roundId));
					return false;
				}
				// A datagram sent again repeats frames this catch-up already holds.
				const uint64_t target = frame.targetFrame;
				if (target > ScenarioRunner::WorldCatchUpAppliedThrough() && !ScenarioRunner::WorldCatchUpHasFrame(target) &&
				    std::none_of(later.begin(), later.end(), [target](const NetLockstepFrame& held) { return held.targetFrame == target; })) {
					later.push_back(std::move(frame));
					++catchUp.tailFramesKept;
				} else ++catchUp.tailFramesRepeated;
				offset += size;
			}
			return true;
		};
		catchUp.tailDatagrams += datagrams.size();
		for (const std::vector<uint8_t>& datagram: datagrams) {
			size_t offset = 0;
			if (!readRecords(datagram, offset)) return;
			if (offset != datagram.size()) {
				ScenarioRunner::SetControllerReplayError("a committed tail datagram ends inside a record"); return;
			}
		}
		size_t offset = 0;
		if (!readRecords(catchUp.partialTail, offset)) return;
		catchUp.partialTail.erase(catchUp.partialTail.begin(), catchUp.partialTail.begin() + offset);
		if (!later.empty()) {
			ScenarioRunner::AppendWorldCatchUp(std::move(later));
		}
		for (NetLobbySession::WorldJoinReport report = lobby.TakeWorldJoinReport(); report.pending; report = lobby.TakeWorldJoinReport()) {
			if (report.kind == c_NetWorldReportRoundEnded) {
				catchUp.endRecord = report.value;
				catchUp.activationTick = RoundEndedFinalFrame(report.value) + 1;
				ScenarioRunner::SetWorldCatchUpActivation(catchUp.activationTick);
				break;
			}
			if (report.kind == c_NetWorldReportRefused) {
				// The world turned this joiner away before any transfer; the caller ends the join.
				if (outRefusal) *outRefusal = report.value;
				return;
			}
			if (report.kind == c_NetWorldReportActivate) {
				catchUp.activationTick = report.value;
				catchUp.activationCommitted = catchUp.privateMatch;
				ScenarioRunner::SetWorldCatchUpActivation(report.value);
				if (!catchUp.privateMatch) lobby.SendPayload(MakeWorldJoinReport(c_NetWorldReportActivationAck, report.value), nullptr);
			}
			if (report.kind == c_NetWorldReportActivationCommit && report.value == catchUp.activationTick) catchUp.activationCommitted = true;
			if (report.kind == c_NetWorldReportHandover && catchUp.privateMatch && !catchUp.handoverCrossed) {
				// Nothing past the handover replays until the replay runs under the authority that committed it.
				catchUp.handover = WorldJoinHandoverFromReport(report.value, report.workTicks, report.workUs, report.sentThrough);
				ScenarioRunner::SetWorldCatchUpFence(catchUp.handover->frame);
			}
		}
		catchUp.appliedThrough = std::max(catchUp.appliedThrough, ScenarioRunner::WorldCatchUpAppliedThrough());
		if (catchUp.endRecord) {
			if (outRoundEnded && catchUp.appliedThrough >= RoundEndedFinalFrame(*catchUp.endRecord)) *outRoundEnded = catchUp.endRecord;
			return;
		}
		// Each progress report carries the whole state of the replay: the newest one wins, so none waits behind a lost one.
		if (catchUp.appliedThrough > catchUp.snapshotTick) {
			const NetLobbyStateChunk report = MakeJoinerCatchUpReport();
			const uint64_t nowMs = NetLockstepNowMs();
			if (report.bytes == catchUp.lastReportBytes && nowMs >= catchUp.reportAttemptMs && nowMs - catchUp.reportAttemptMs < 250) return;
			catchUp.lastReportBytes = report.bytes;
			catchUp.reportAttemptMs = nowMs;
			std::string sendError;
			if (lobby.SendPayload(report, &sendError, NetTransportLane::BulkUnreliable)) ++catchUp.reportsSent;
			else if (catchUp.reportsRefused++ == 0) System::PrintDiagnosticLine("[net-match] catch-up report refused by the wire: " + sendError);
			if (catchUp.appliedThrough >= catchUp.reportsLogged + 60) {
				catchUp.reportsLogged = catchUp.appliedThrough;
				System::PrintDiagnosticLine("[net-match] catch-up reports applied=" + std::to_string(catchUp.appliedThrough) + " sent=" + std::to_string(catchUp.reportsSent) +
				                            " refused=" + std::to_string(catchUp.reportsRefused) + " datagrams=" + std::to_string(catchUp.tailDatagrams) +
				                            " reports_sent=" + std::to_string(lobby.GetStats().catchUpReportsSent) + " reports_received=" + std::to_string(lobby.GetStats().catchUpReportsReceived) +
				                            " reports_refused=" + std::to_string(lobby.GetStats().catchUpReportsRefused) + " reports_dropped=" + std::to_string(lobby.GetStats().catchUpReportsDropped));
			}
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
			{
				std::ostringstream line;
				line << "[net-world] directory token not persisted: " << writeError;
				System::PrintDiagnosticLine(line.str());
			}
		}
	}

	bool NetMatchService::ReleaseWorldCatchUpOnceRunning(bool coordinatorRunning, NetWorldCatchUpClient& catchUp) {
		// A replay that handed the sim to its round is over even if that round stopped again in the same pass: from here the
		// round's own record of holds and returns applies them, through the held seat's catch-up if it is held again.
		if (!(coordinatorRunning || catchUp.handedToRound) || !catchUp.active) {
			return false;
		}
		// The coordinator owns the wire and the pacing from the moment it runs. A catch-up left armed
		// past that drains the round's own packets into the session and keeps reporting a finished
		// bootstrap the host answers with "that bootstrap is not catching up".
		catchUp = {};
		ScenarioRunner::ReleaseWorldCatchUp();
		return true;
	}

	void NetMatchService::AdoptWorldTicketSession(const NetMatchConfig& config) {
		if (m_IsHost || !config.persistentWorld || !NetMatchConfigUtil::IsWorldId(config.worldId)) {
			return;
		}
		// An address-only join learns the world's ticket context from the adopted lobby config.
		m_LastJoinTargetPersistentWorld = true;
		m_ReconnectClient.SetWorldTarget(true);
		m_ReconnectClient.AdoptDirectorySessionId(config.worldId);
	}

	bool NetMatchService::BeginInPlaceCatchUp() {
		std::lock_guard<std::mutex> lock(m_Mutex);
		const LockedLobbyDrive lockedDrive(*this);
		if (m_IsHost || m_State != NetMatchServiceState::Running || !m_Coordinator || !m_Runner || !m_Session || m_WorldCatchUp.active || !ActiveWireLocked() ||
		    !m_Coordinator->IsStopped() || !m_Coordinator->IsLocalSeatHeld() || !m_Coordinator->UsesBoundedWait() || m_Coordinator->IsPersistentWorldRound() ||
		    m_Session->IsFailed() || m_Session->GetState() == NetSessionState::Closed || m_Session->GetState() == NetSessionState::Rejected || !g_ActivityMan.ActivityRunning()) return false;
		const uint64_t holdFrame = m_Coordinator->GetLocalHoldFrame();
		const uint64_t tick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
		// A sim that ran the hold frame played input the round never committed; only one short of it holds the round's state.
		if (holdFrame == 0 || tick == 0 || tick >= holdFrame) {
			System::PrintDiagnosticLine("[net-match] held client: no in-place catch-up (sim at " + std::to_string(tick) + ", held from " + std::to_string(holdFrame) + ")");
			return false;
		}
		NetResyncState committed;
		std::string error;
		if (!ScenarioRunner::CaptureNetResyncState(tick, committed, &error, false)) {
			System::PrintDiagnosticLine("[net-match] held client: no in-place catch-up: " + error);
			return false;
		}
		committed.pendingInputs.clear(); committed.pendingCommands.clear(); committed.pendingPlayerBindings.clear(); committed.admittedReseats.clear();
		const NetLockstepPauseState pause = ScenarioRunner::CaptureLockstepPauseState();
		const NetLockstepConfig& live = m_Coordinator->GetConfig();
		NetLockstepConfig config;
		config.sessionId = live.sessionId;
		config.roundId = m_Coordinator->GetRoundId();
		config.matchConfig = live.matchConfig;
		config.peerCount = live.peerCount; config.localPeerId = live.localPeerId;
		config.authorityPeerId = m_Coordinator->GetHostPeerId();
		config.startFrame = tick + 1;
		config.migrationGeneration = live.migrationGeneration;
		config.migrationKey = live.migrationKey;
		config.migrationTransportFactory = live.migrationTransportFactory;
		config.migrationIceDial = live.migrationIceDial;
		config.migrationIceHost = live.migrationIceHost;
		config.originalRoundConfigHash = m_Coordinator->GetRoundConfigHash();
		config.simTickMs = g_TimerMan.GetDeltaTimeMS();
		for (const auto& [peer, frame]: m_Coordinator->GetPeerLeaveFrames()) if (frame <= tick) config.initialPeerLeaves[peer] = frame;
		for (const auto& [peer, hold]: m_Coordinator->HeldTransactions()) if (hold.cutoffFrame <= tick) config.initialSeatHolds[peer] = hold;
		auto transport = std::make_unique<LoopbackTransport>();
		auto replay = std::make_unique<NetLockstepCoordinator>();
		if (!replay->StartReplay(*transport, config, &error)) {
			System::PrintDiagnosticLine("[net-match] held client: no in-place catch-up: " + error);
			return false;
		}
		const uint64_t priorInput = m_Coordinator->SentInputThrough();
		m_CatchUpTransport = std::move(transport);
		m_CatchUpCoordinator = std::move(replay);
		ScenarioRunner::SetLockstepCoordinator(m_CatchUpCoordinator.get());
		if (!ScenarioRunner::RestoreCommittedCatchUpState(committed, &error) || !ScenarioRunner::RestoreLockstepPauseState(pause, tick) ||
		    !ScenarioRunner::InstallWorldCatchUp(tick, {}, &error, true)) {
			// The stopped round goes back, and its stop reloads the image as a relaunch would.
			ScenarioRunner::SetLockstepCoordinator(m_Coordinator.get(), true);
			m_CatchUpCoordinator.reset(); m_CatchUpTransport.reset();
			System::PrintDiagnosticLine("[net-match] held client: no in-place catch-up: " + error);
			return false;
		}
		ScenarioRunner::SetWorldCatchUpPriorInputThrough(priorInput);
		m_WorldCatchUp = {};
		m_WorldCatchUp.active = true;
		m_WorldCatchUp.privateMatch = true;
		m_WorldCatchUp.roundId = config.roundId;
		m_WorldCatchUp.authorityGeneration = config.migrationGeneration;
		m_WorldCatchUp.authorityPeerId = config.authorityPeerId;
		m_WorldCatchUp.initialPeerLeaves = config.initialPeerLeaves;
		m_WorldCatchUp.checkpointConfig = config.matchConfig;
		m_WorldCatchUp.roundConfigHash = m_Coordinator->GetRoundConfigHash();
		m_WorldCatchUp.pauseState = pause;
		m_WorldCatchUp.initialHolds = config.initialSeatHolds;
		m_WorldCatchUp.snapshotTick = m_WorldCatchUp.appliedThrough = tick;
		m_CatchUpWirePackets.clear(); m_CatchUpWireBytes = 0;
		m_ActivateCatchUpLocalSeat = {};
		m_InPlaceCatchUp = true;
		m_InPlaceAskedMs = m_InPlaceSinceMs = m_InPlaceHeardMs = SteadyNowMs();
		m_HeldRecordPackets.clear(); m_HeldRecordQueued = false; m_HeldGatherSinceMs = 0;
		m_InPlaceProgressApplied = m_InPlaceProgressLogged = tick;
		// The successors this catch-up moves to, in the order the match published, if its host goes.
		m_InPlaceRoutes.clear(); m_InPlaceMoveHost = 0;
		for (const uint8_t peer: live.matchConfig.successorOrder) {
			if (peer == config.authorityPeerId || peer == m_LocalPeerId) continue;
			const auto endpoint = std::find_if(live.matchConfig.migrationPeers.begin(), live.matchConfig.migrationPeers.end(), [&](const auto& candidate) { return candidate.peerId == peer; });
			if (endpoint != live.matchConfig.migrationPeers.end() && endpoint->listenPort != 0 && !endpoint->listenAddrs.empty()) m_InPlaceRoutes.push_back({peer, *endpoint});
		}
		// The first survivor in the match's order listens from its hold on: a held seat that finds the host gone before it does dials it then,
		// not after this seat's own link times out.
		{
			std::vector<uint8_t> reachable;
			for (const InPlaceRoute& route: m_InPlaceRoutes) reachable.push_back(route.peerId);
			std::set<uint8_t> departed;
			for (const auto& [peer, frame]: config.initialPeerLeaves)
				if (peer != config.authorityPeerId && peer != m_LocalPeerId && !config.initialSeatHolds.contains(peer)) departed.insert(peer);
			if (!reachable.empty() && HeldSeatListens(live.matchConfig.successorOrder, config.authorityPeerId, m_LocalPeerId, reachable, {}, departed)) (void)OpenHeldListenerLocked();
		}
		NoteTailReplayBeganLocked();
		(void)m_Runner->GetLobbySession().SendPayload(MakeJoinerCatchUpReport(), nullptr);
		ScenarioRunner::PushNetUiToast("seat_held", "Held - AI in control - rejoining");
		System::PrintDiagnosticLine("[net-match] held client: catching up in place from frame " + std::to_string(tick) + " (held from " + std::to_string(holdFrame) +
		                            ", input sent through " + std::to_string(priorInput) + ")");
		return true;
	}

	std::set<uint8_t> NetMatchService::HeldSurvivorsLocked() const {
		std::set<uint8_t> held;
		if (!m_Coordinator || !m_CatchUpCoordinator) return held;
		// The seats this replay holds, and those the stopped round held from a frame the replay has not reached.
		for (const auto& [peer, hold]: m_CatchUpCoordinator->HeldTransactions())
			if (m_CatchUpCoordinator->HasHeldAISeat(peer)) held.insert(peer);
		for (const auto& [peer, hold]: m_Coordinator->HeldTransactions())
			if (hold.cutoffFrame > m_WorldCatchUp.appliedThrough) held.insert(peer);
		held.erase(m_LocalPeerId);
		held.erase(m_Coordinator->GetHostPeerId());
		return held;
	}

	size_t NetMatchService::HumanSeatCountLocked() const {
		if (!m_Coordinator) return 0;
		std::set<uint8_t> humans;
		for (const NetMatchPlayerSlot& slot: m_Coordinator->GetConfig().matchConfig.players)
			if (!slot.cpu && slot.peerId != 0) humans.insert(slot.peerId);
		return humans.size();
	}

	bool NetMatchService::HeldSeatHostsLocked() {
		m_HeldUnreachableText.clear();
		if (!m_Coordinator || !m_CatchUpCoordinator) {
			System::PrintDiagnosticLine("[net-match] held client: its host is gone and its catch-up is gone too");
			return false;
		}
		std::vector<uint8_t> reachable;
		for (const InPlaceRoute& route: m_InPlaceRoutes) reachable.push_back(route.peerId);
		const std::set<uint8_t> held = HeldSurvivorsLocked();
		const uint8_t lostHost = m_Coordinator->GetHostPeerId();
		const std::vector<uint8_t>& order = m_Coordinator->GetConfig().matchConfig.successorOrder;
		const std::vector<uint8_t> routes = HeldSuccessionRoutes(order, lostHost, m_LocalPeerId, reachable, held);
		// The seats the lost host held, as this seat heard them: its last revision of them is what it knows.
		uint64_t lastRevision = 0;
		std::set<uint8_t> departed;
		for (const auto& holds: {m_CatchUpCoordinator->HeldTransactions(), m_Coordinator->HeldTransactions()})
			for (const auto& [peer, hold]: holds) lastRevision = std::max(lastRevision, hold.eventSequence);
		for (const auto& [peer, frame]: m_CatchUpCoordinator->GetPeerLeaveFrames())
			if (peer != lostHost && peer != m_LocalPeerId && !held.contains(peer) && !m_CatchUpCoordinator->HeldTransactions().contains(peer)) departed.insert(peer);
		const bool listens = HeldSeatListens(order, lostHost, m_LocalPeerId, reachable, held, departed);
		std::deque<InPlaceRoute> kept;
		for (const uint8_t peer: routes)
			kept.push_back(*std::find_if(m_InPlaceRoutes.begin(), m_InPlaceRoutes.end(), [&](const InPlaceRoute& route) { return route.peerId == peer; }));
		m_InPlaceRoutes = std::move(kept);
		std::ostringstream line;
		line << "[net-match] held client: its host is gone; last_revision=" << lastRevision << " held=";
		for (const uint8_t peer: held) line << static_cast<int>(peer) << ' ';
		line << "routes=";
		for (const uint8_t peer: routes) line << static_cast<int>(peer) << ' ';
		line << "listens=" << listens << ' ';
		line << (routes.empty() ? "- this seat hosts the match" : routes.size() == 1 && held.contains(routes.front()) ? "- every survivor is held; the first of them hosts" : "");
		line << " clock=" << SteadyNowMs();
		System::PrintDiagnosticLine(line.str());
		if (!routes.empty() && listens) (void)OpenHeldListenerLocked();
		// The seats the lost host held have no vote: beside other players they wait for the host, which may be the one still playing.
		if (routes.empty() && HumanSeatCountLocked() > 2) {
			m_HeldUnreachableText = "PeerHeld:The host is unreachable - 1 of " + std::to_string(HumanSeatCountLocked()) + " players reachable";
			System::PrintDiagnosticLine("[net-match] held client: no majority can carry the match without its host; it waits for the host");
			return false;
		}
		return routes.empty();
	}

	bool NetMatchService::OpenHeldListenerLocked() {
		if (m_HeldListener) return true;
		m_HeldDialSeen = m_HeldDialNoted = false;
		m_HeldGatherSinceMs = 0; m_HeldRecordsEnded.clear(); m_HeldRecordFramesTaken = m_HeldRecordThrough = 0;
		const NetLockstepConfig& config = m_Coordinator->GetConfig();
		const auto endpoint = std::find_if(config.matchConfig.migrationPeers.begin(), config.matchConfig.migrationPeers.end(), [&](const auto& peer) { return peer.peerId == m_LocalPeerId; });
		std::unique_ptr<INetTransport> listener = config.migrationTransportFactory ? config.migrationTransportFactory() : nullptr;
		if (endpoint == config.matchConfig.migrationPeers.end() || !listener || !listener->StartHost(endpoint->listenPort)) {
			System::PrintDiagnosticLine("[net-match] held client: its listener could not open; it only dials");
			return false;
		}
		if (config.migrationIceHost) config.migrationIceHost(*listener);
		m_HeldListener = std::move(listener);
		m_HeldListenerEvents.clear();
		System::PrintDiagnosticLine("[net-match] held client: the first survivor listens on " + std::to_string(endpoint->listenPort) + " while it dials");
		return true;
	}

	void NetMatchService::CloseHeldListenerLocked() {
		m_HeldDialSeen = m_HeldDialNoted = false;
		m_HeldGatherSinceMs = 0; m_HeldRecordsEnded.clear();
		if (!m_HeldListener) return;
		m_HeldListener->Stop();
		m_HeldListener.reset();
		m_HeldListenerEvents.clear();
	}

	bool NetMatchService::HostAloneFromOwnStateLocked() {
		if (HumanSeatCountLocked() > 2) {
			System::PrintDiagnosticLine("[net-match] held client cannot host alone: " + std::to_string(HumanSeatCountLocked()) + " players and no majority without the host");
			return false;
		}
		if (!m_Coordinator || !m_CatchUpCoordinator || !m_CatchUpTransport || !g_ActivityMan.ActivityRunning()) {
			System::PrintDiagnosticLine(std::string("[net-match] held client cannot host alone: ") + (!m_CatchUpCoordinator || !m_CatchUpTransport ? "its catch-up is gone" : "no round runs"));
			return false;
		}
		const uint64_t tick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
		NetResyncState committed;
		std::string error;
		if (!ScenarioRunner::CaptureNetResyncState(tick, committed, &error, false)) {
			System::PrintDiagnosticLine("[net-match] held client cannot host alone: " + error);
			return false;
		}
		committed.pendingInputs.clear(); committed.pendingCommands.clear(); committed.pendingPlayerBindings.clear(); committed.admittedReseats.clear();
		const NetLockstepPauseState pause = ScenarioRunner::CaptureLockstepPauseState();
		// The round goes on from the tick this peer stands on, with every other seat the AI's: its own hold ends, as nobody is left to wait for.
		NetLockstepConfig config = m_Coordinator->GetConfig();
		const uint8_t local = config.localPeerId;
		const uint8_t lostHost = m_Coordinator->GetHostPeerId();
		// Held seats that will rejoin this peer need it listening where the match published it, as an elected host does.
		std::unique_ptr<INetTransport> listener;
		uint64_t ownReturn = 0, handoverFrame = 0;
		if (m_HeldListener) {
			listener = std::make_unique<EarlyListener>(std::move(m_HeldListener), std::move(m_HeldListenerEvents));
			m_HeldListenerEvents.clear();
		} else if (!HeldSurvivorsLocked().empty()) {
			const auto endpoint = std::find_if(config.matchConfig.migrationPeers.begin(), config.matchConfig.migrationPeers.end(), [&](const auto& peer) { return peer.peerId == local; });
			listener = config.migrationTransportFactory ? config.migrationTransportFactory() : nullptr;
			if (endpoint == config.matchConfig.migrationPeers.end() || !listener || !listener->StartHost(endpoint->listenPort)) {
				System::PrintDiagnosticLine("[net-match] held client: its handover listener could not open; it hosts alone");
				listener.reset();
			} else if (config.migrationIceHost) config.migrationIceHost(*listener);
		}
		config.roundId = m_Coordinator->GetRoundId();
		config.originalRoundConfigHash = m_Coordinator->GetRoundConfigHash();
		config.startFrame = tick + 1;
		config.authorityPeerId = local;
		config.activePeerIds = {local};
		config.remoteTransportPeerIds.clear();
		config.remotePeerId = 0;
		config.remoteTransportPeerId = c_InvalidNetPeerId;
		config.relayToOtherPeers = true;
		config.requirePublishedStart = false;
		config.resumeFromSnapshot = false;
		config.joinsRunningRound = false;
		// Every peer that replays this round reaches its first frames with the routes the lost host's committed frames left: this one keeps them too.
		config.continuesMatch = true;
		config.initialPeerLeaves.clear();
		config.initialSeatHolds.clear();
		config.initialSeatReclaims.clear();
		for (const auto& [peer, frame]: m_CatchUpCoordinator->GetPeerLeaveFrames()) if (peer != local && frame <= tick) config.initialPeerLeaves[peer] = frame;
		for (const auto& [peer, hold]: m_CatchUpCoordinator->HeldTransactions()) if (peer != local && hold.cutoffFrame <= tick) config.initialSeatHolds[peer] = hold;
		config.initialDelayChanges = m_CatchUpCoordinator->GetDelayChanges();
		// The delays the lost host committed before this seat's hold are the round's too, its own seat's included: every seat that returns expects them.
		for (const auto& [peer, changes]: m_Coordinator->GetDelayChanges())
			for (const auto& [frame, delay]: changes) config.initialDelayChanges[peer].emplace(frame, delay);
		if (listener) {
			// The round's first frame past its input-delay prefix is the first it builds and the first its authority names: the round
			// changes hands there. The lost host leaves there, the prefix before it replays under the lost host on every peer, and a
			// returner crosses there.
			const auto delayOf = [&](uint8_t peer) {
				const auto found = config.peerInputDelayFrames.find(peer);
				return found != config.peerInputDelayFrames.end() ? found->second : config.inputDelayFrames;
			};
			uint64_t firstBuilt = config.startFrame + config.inputDelayFrames;
			for (uint8_t peer = 1; peer <= config.peerCount; ++peer) firstBuilt = std::min<uint64_t>(firstBuilt, config.startFrame + delayOf(peer));
			handoverFrame = firstBuilt;
			config.initialPeerLeaves[lostHost] = handoverFrame;
			++config.migrationGeneration;
			// Its own seat comes back on the committed stream at the handover, as any held seat's return does, so every seat that
			// rejoins it takes its actors back from the AI at the same frame.
			std::optional<NetGameSeatHold> ownHold;
			for (const auto& holds: {m_CatchUpCoordinator->HeldTransactions(), m_Coordinator->HeldTransactions()})
				if (!ownHold && holds.contains(local)) ownHold = holds.at(local);
			const uint16_t delay = delayOf(local);
			ownReturn = ownHold ? handoverFrame : 0;
			if (ownHold) {
				const uint32_t incarnation = std::max(ownHold->seatIncarnation, config.peerIncarnations.contains(local) ? config.peerIncarnations.at(local) : 0U) + 1;
				config.initialSeatHolds[local] = *ownHold;
				config.peerIncarnations[local] = incarnation;
				config.initialSeatReclaims[local] = {local, config.migrationGeneration, ownHold->eventSequence + 1, incarnation, ownReturn, delay, ownReturn + delay, std::nullopt};
			}
			// A survivor whose hold this seat never heard is held all the same (only a held seat dials it): the round holds it from the
			// handover on every peer, as its own hold, and its return is this round's to agree.
			for (uint8_t peer = 1; peer <= config.peerCount; ++peer) {
				if (peer == local || peer == lostHost || config.initialSeatHolds.contains(peer) || config.initialPeerLeaves.contains(peer)) continue;
				NetGameSeatHold hold;
				hold.peerId = peer;
				hold.authorityGeneration = config.migrationGeneration;
				hold.eventSequence = 1;
				hold.seatIncarnation = config.peerIncarnations.contains(peer) ? config.peerIncarnations.at(peer) : 1;
				hold.cutoffFrame = handoverFrame;
				config.initialSeatHolds[peer] = hold;
				config.initialPeerLeaves[peer] = handoverFrame;
				System::PrintDiagnosticLine("[net-match] held client: peer=" + std::to_string(peer) + " is held from the handover at " + std::to_string(handoverFrame) + " (its hold never reached this seat)");
			}
		}
		if (!m_Coordinator->Start(listener ? *listener : *m_CatchUpTransport, config, &error) || !m_Coordinator->IsRunning()) {
			System::PrintDiagnosticLine("[net-match] held client cannot host alone: " + (error.empty() ? std::string("its round did not start") : error));
			return false;
		}
		ScenarioRunner::SetLockstepCoordinator(m_Coordinator.get(), true);
		const bool listening = listener != nullptr;
		if (listening) {
			if (INetTransport* wire = ActiveWireLocked()) wire->Stop();
			m_MigratedTransport = std::move(listener);
		}
		if (!ScenarioRunner::RestoreCommittedCatchUpState(committed, &error) || !ScenarioRunner::RestoreLockstepPauseState(pause, tick)) {
			ScenarioRunner::SetControllerReplayError("PeerLeft:the held seat could not host the match alone: " + error);
			return true;
		}
		m_CatchUpCoordinator.reset();
		m_CatchUpWirePackets.clear(); m_CatchUpWireBytes = 0;
		m_ActivateCatchUpLocalSeat = {};
		m_InPlaceCatchUp = false;
		m_WorldCatchUp = {};
		ScenarioRunner::ReleaseWorldCatchUp();
		ScenarioRunner::NoteLocalSeatReclaimed();
		m_Coordinator->DeferStopsToTickBoundary();
		const bool hostsHeldSeats = listening && OpenHeldHostPlaneLocked(handoverFrame, lostHost);
		if (hostsHeldSeats) m_CatchUpTransport.reset();
		std::ostringstream line;
		line << "[net-match] Host left - " << (m_LocalName.empty() ? std::string("this peer") : m_LocalName) << " is now hosting; boundary=" << tick
		     << " round=" << config.roundId << " alone=" << !hostsHeldSeats << " lost_host=" << static_cast<int>(lostHost) << " own_return=" << ownReturn
		     << " agreed=" << m_Coordinator->HasAgreedSeatReclaim(local) << " held=" << m_Coordinator->HasHeldAISeat(local) << " next=" << m_Coordinator->GetStats().nextFrame;
		System::PrintDiagnosticLine(line.str());
		if (!hostsHeldSeats) {
			m_StatusText = "Hosting alone - AI in control of the other seats";
			ScenarioRunner::PushNetUiToast("host_handover_live", "Host left - hosting the match alone");
		}
		return true;
	}

	bool NetMatchService::OpenHeldHostPlaneLocked(uint64_t handoverFrame, uint8_t lostHost) {
		// The admission, the lobby and the rejoin plane an elected host takes over, on the listener the round now runs on.
		const NetMatchConfig& config = m_Coordinator->GetConfig().matchConfig;
		const uint64_t nowMs = AdmissionNowMs();
		m_MigrationAuthority = m_LocalPeerId;
		m_MigrationMembers = {m_LocalPeerId};
		m_MigrationGeneration = m_Coordinator->GetConfig().migrationGeneration;
		m_IsHost = true;
		m_HandoverFrame = handoverFrame;
		m_PendingModeration.clear();
		m_PendingHostOptions.reset();
		m_HostOptionsRequest.Clear();
		m_LastRemovalIssue = {};
		m_LastKickBanResult = NetKickBanResult::ActionUnavailable;
		m_BanStore.SetPath(NetHostBanStore::DefaultPath());
		(void)m_BanStore.Load(nullptr);
		m_ReconnectHost.SetBanStore(&m_BanStore);
		m_ReconnectHost.SetParticipantProofRequired(true);
		m_ReconnectHost.SetPersistentWorld(config.persistentWorld);
		m_ReconnectHost.SetDropOwnershipSource(&NetMatchService::CollectDropOwnership, this);
		m_ReconnectHost.SetSeatSimIdentitySource(&NetMatchService::SeatSimIdentitySource, this);
		m_PendingSessionEvents.clear();
		if (!m_ReconnectHost.ImportMigrationState(m_MigrationAdmissionState, m_SeatAuth, config, m_LocalPeerId, {}, nowMs) ||
		    !m_Session->AdoptHostMigration(*m_MigratedTransport, m_LocalPeerId, m_LocalPeerId, config, {}, nowMs)) {
			System::PrintDiagnosticLine("[net-match] held client: the match's admission state did not carry over; it hosts alone");
			m_IsHost = false;
			return false;
		}
		m_ChatSession = m_Session.get();
		m_Session->SetReconnectClient(nullptr);
		m_Session->SetReconnectHost(&m_ReconnectHost);
		m_Session->SetHostBanStore(&m_BanStore);
		m_Session->EnableParticipantProof(nullptr);
		m_ReconnectHost.SetLiveMatch(true);
		m_ReconnectHost.SetMigrationHold(false, nowMs);
		m_ModerationSeats = m_ReconnectHost.GetModerationView();
		{
			const SimCensusScope census;
			m_ReconnectHost.RecordMigrationDepartures(handoverFrame);
		}
		NetHostMigrationResult result;
		result.generation = m_MigrationGeneration;
		result.boundary = handoverFrame - 1;
		result.hostPeerId = m_LocalPeerId;
		result.members = {m_LocalPeerId};
		m_Runner->AdoptHostMigration(result, m_LocalPeerId);
		NetLobbySessionConfig lobby;
		lobby.host = true;
		lobby.localPeerId = m_LocalPeerId;
		lobby.matchConfig = config;
		lobby.startFrame = handoverFrame;
		lobby.session = m_Session.get();
		lobby.sessionNowMs = [this] { return AdmissionNowMs(); };
		lobby.displayName = m_LocalName.empty() ? "Host" : m_LocalName;
		lobby.autoStart = false;
		std::string lobbyError;
		if (!m_Runner->GetLobbySession().Start(*m_MigratedTransport, lobby, &lobbyError))
			System::PrintDiagnosticLine("[net-match] the new host's rejoin lobby did not open: " + lobbyError);
		m_ResyncOnDesync = true;
		const auto endpoint = std::find_if(config.migrationPeers.begin(), config.migrationPeers.end(), [&](const auto& peer) { return peer.peerId == m_LocalPeerId; });
		if (endpoint != config.migrationPeers.end()) {
			m_DirectoryRow.listenAddrs = endpoint->listenAddrs;
			std::erase_if(m_DirectoryRow.listenAddrs, [](const std::string& address) { return NetLockstepCoordinator::IsMigrationIceEndpoint(address); });
			m_DirectoryRow.listenPort = endpoint->listenPort;
			m_DirectoryRow.joinMode = "ip";
			m_DirectoryRow.resumeSessionId = m_MigrationDirectorySession;
			m_DirectoryRow.resumeToken = m_MigrationDirectoryToken;
			m_DirectoryRow.name = m_LocalName;
			m_DirectoryRow.peerCount = 1;
			m_DirectoryRetracted = false;
			m_MigrationDirectoryResumePending = !m_MigrationDirectorySession.empty();
			m_BeaconGamePort = endpoint->listenPort;
		}
		m_IceRoute = "ip";
		for (auto& member: m_LobbySnapshot.members) member.connected = member.cpu || member.peerId == m_LocalPeerId;
		// The seats it hosts are still reconnecting: the loss reads as a handover until they have had the time to.
		m_StatusText = "Host lost - arranging handover";
		m_HeldHostStatusAtMs = SteadyNowMs() + c_HeldHostArrangingMs;
		System::PrintDiagnosticLine("[net-match] held client hosts the held seats: lost_host=" + std::to_string(lostHost) + " handover=" + std::to_string(m_HandoverFrame) +
		                            " generation=" + std::to_string(m_MigrationGeneration));
		return true;
	}

	bool NetMatchService::BindClientLobbyToHostLocked(INetTransport& wire, uint8_t hostPeerId, NetPeerId hostLink, uint64_t startFrame) {
		if (!m_Runner || !m_Session || hostPeerId == 0 || hostLink == c_InvalidNetPeerId) return false;
		NetLobbySessionConfig lobby;
		lobby.host = false;
		lobby.localPeerId = m_LocalPeerId;
		lobby.remoteTransportPeerIds = {{hostPeerId, hostLink}};
		lobby.matchConfig = m_Coordinator ? m_Coordinator->GetConfig().matchConfig : m_Runner->GetMatchConfig();
		lobby.startFrame = startFrame;
		lobby.session = m_Session.get();
		lobby.sessionNowMs = [this] { return AdmissionNowMs(); };
		lobby.displayName = m_LocalName.empty() ? "Client" : m_LocalName;
		// A running round never names this seat again, so the lobby says nothing of its own.
		lobby.assignSeats = true;
		lobby.autoReady = false;
		lobby.autoStart = false;
		std::string error;
		if (!m_Runner->GetLobbySession().Start(wire, lobby, &error)) {
			System::PrintDiagnosticLine("[net-match] the lobby did not follow host peer=" + std::to_string(hostPeerId) + ": " + error);
			return false;
		}
		return true;
	}

	void NetMatchService::DropReturnStartLocked(const std::string& why) {
		if (m_Runner) m_Runner->CancelWorldJoinLockstepStart();
		if (m_Coordinator) {
			// The decisions for the other seats that round heard before its start are the round's own; the next start takes them.
			for (const auto& [timing, transport]: m_Coordinator->TakePreStartTiming()) {
				NetTransportEvent event;
				event.type = NetTransportEventType::PacketReceived; event.peerId = transport; event.lane = NetTransportLane::ControlReliable;
				if (!NetLockstepCodec::Encode({timing}, event.bytes)) continue;
				m_CatchUpWireBytes += event.bytes.size();
				m_CatchUpWirePackets.push_back(std::move(event));
			}
			if (m_Coordinator->IsRunning()) m_Coordinator->Complete(why);
		}
		// Every start on the wire answered the return that is over.
		std::erase_if(m_CatchUpWirePackets, [this](const NetTransportEvent& event) {
			const bool start = event.bytes.size() >= NetLockstepCodec::c_HeaderBytes && event.bytes[8] == static_cast<uint8_t>(NetLockstepPacketType::Start);
			if (start) m_CatchUpWireBytes -= event.bytes.size();
			return start;
		});
	}

	bool NetMatchService::BeginInPlaceMoveLocked(uint64_t nowMs) {
		if (!m_InPlaceCatchUp || !m_Coordinator || !m_Session || !m_Runner || !m_CatchUpCoordinator || m_InPlaceRoutes.empty()) return false;
		// A return the lost host agreed passes without this seat: the successor holds it again there and agrees its own.
		if (m_WorldCatchUp.activationTick != 0) {
			System::PrintDiagnosticLine("[net-match] held client: its return at " + std::to_string(m_WorldCatchUp.activationTick) + " was agreed with the lost host; the successor agrees the next");
			if (m_Runner->IsWorldJoinLockstepStarting()) m_Runner->CancelWorldJoinLockstepStart();
			if (m_Coordinator->IsRunning()) m_Coordinator->Complete("the host that agreed this seat's return is gone");
			m_WorldCatchUp.activationTick = 0;
			m_WorldCatchUp.activationCommitted = false;
			ScenarioRunner::SetWorldCatchUpActivation(0);
		}
		m_CatchUpWirePackets.clear(); m_CatchUpWireBytes = 0;
		return DialNextInPlaceRouteLocked(nowMs);
	}

	bool NetMatchService::DialNextInPlaceRouteLocked(uint64_t nowMs) {
		m_InPlaceMoveHost = 0;
		m_TicketStore.SetPath(s_TicketStorePath.empty() ? NetReconnectTicketStore::DefaultPath() : s_TicketStorePath);
		if (m_InPlaceTicketHost.empty()) {
			NetH4TicketRecord record;
			if (m_TicketStore.Load(UnixNowMs(nullptr), record, nullptr) == NetH4TicketLoadResult::Loaded) m_InPlaceTicketHost = record.hostAddress;
		}
		while (!m_InPlaceRoutes.empty()) {
			const InPlaceRoute route = m_InPlaceRoutes.front();
			m_InPlaceRoutes.pop_front();
			const std::string& address = route.endpoint.listenAddrs.front();
			// The seat's ticket names the match's host; the successor holds the match's admission state and answers it.
			NetH4TicketRecord record;
			if (m_TicketStore.Load(UnixNowMs(nullptr), record, nullptr) == NetH4TicketLoadResult::Loaded && record.hostAddress != address) {
				record.hostAddress = address;
				(void)m_TicketStore.Store(record, nullptr);
			}
			NetSessionConfig sessionConfig = m_Session->GetConfig();
			sessionConfig.port = route.endpoint.listenPort;
			sessionConfig.p2pJoin.connect = [this, peer = route.endpoint](INetTransport& transport, std::string* connectError) {
				size_t nextAddress = 0;
				std::string connectedAddress;
				const NetLockstepCoordinator::MigrationIceDial dial = [this](INetTransport& link, const std::string& identity, std::string* dialError) { return DialMigrationIce(link, identity, dialError); };
				return NetLockstepCoordinator::ConnectMigrationEndpoint(transport, peer, nextAddress, connectedAddress, connectError, &dial);
			};
			NetMatchServiceRequest request;
			request.host = false;
			request.address = address;
			request.port = route.endpoint.listenPort;
			request.playerName = m_LocalName;
			request.rejoin = true;
			request.sessionId = !m_MigrationDirectorySession.empty() ? m_MigrationDirectorySession : m_LastJoinRoute ? m_LastJoinRoute->sessionId : std::string();
			std::string error;
			auto transport = std::make_unique<GnsTransport>();
			if (!AttachAdmissionPlane(*m_Session, request, m_Coordinator->GetConfig().matchConfig, sessionConfig, sessionConfig.localIdentity, &error) ||
			    !m_Session->StartClient(*transport, address, sessionConfig, &error)) {
				System::PrintDiagnosticLine("[net-match] held client: the successor at " + address + ":" + std::to_string(route.endpoint.listenPort) + " could not be dialed: " + error);
				continue;
			}
			m_MigratedTransport = std::move(transport);
			m_InPlaceMoveHost = route.peerId;
			m_InPlaceMoveAddress = address + ":" + std::to_string(route.endpoint.listenPort);
			m_InPlaceMoveSinceMs = SteadyNowMs();
			m_HeldRecordPackets.clear(); m_HeldRecordQueued = false;
			std::ostringstream line;
			line << "[net-match] held client: the host is gone; the catch-up moves to the successor at " << m_InPlaceMoveAddress << " peer=" << static_cast<int>(route.peerId)
			     << " from=" << m_WorldCatchUp.appliedThrough;
			System::PrintDiagnosticLine(line.str());
			return true;
		}
		// No successor took the seat: its ticket names the host it named before, for the rejoin that follows.
		NetH4TicketRecord record;
		if (!m_InPlaceTicketHost.empty() && m_TicketStore.Load(UnixNowMs(nullptr), record, nullptr) == NetH4TicketLoadResult::Loaded && record.hostAddress != m_InPlaceTicketHost) {
			record.hostAddress = m_InPlaceTicketHost;
			(void)m_TicketStore.Store(record, nullptr);
		}
		m_InPlaceTicketHost.clear();
		return false;
	}

	void NetMatchService::SendHeldRecordLocked() {
		const NetPeerId link = m_Session ? m_Session->GetRemoteTransportPeerId() : c_InvalidNetPeerId;
		if (!m_MigratedTransport || !m_InPlaceCatchUp || link == c_InvalidNetPeerId) return;
		if (!m_HeldRecordQueued) {
			m_HeldRecordQueued = true;
			// The lost host's frames this seat replayed are the round's own: a successor holding fewer of them takes them before it hosts.
			std::vector<std::vector<uint8_t>> records;
			const uint64_t from = m_WorldCatchUp.snapshotTick + 1;
			if (m_CommittedRing.Count() > 0 && m_CommittedRing.LastFrame() >= from) {
				const uint64_t newest = m_CommittedRing.LastFrame() >= from + c_HeldRecordFrames ? m_CommittedRing.LastFrame() + 1 - c_HeldRecordFrames : from;
				(void)m_CommittedRing.CopyFrom(newest, c_HeldRecordFrames, 16ULL * 1024 * 1024, records);
			}
			std::vector<uint8_t> slice;
			const auto pack = [&] {
				std::vector<uint8_t> bytes;
				if (NetLobbyProtocol::Encode({MakeWorldTailChunk(m_WorldCatchUp.roundId, slice)}, bytes)) m_HeldRecordPackets.push_back(std::move(bytes));
				slice.clear();
			};
			for (const std::vector<uint8_t>& record: records) {
				if (!slice.empty() && slice.size() + 4 + record.size() > c_HeldRecordChunkBytes) pack();
				const uint32_t size = static_cast<uint32_t>(record.size());
				for (int shift = 0; shift < 32; shift += 8) slice.push_back(static_cast<uint8_t>(size >> shift));
				slice.insert(slice.end(), record.begin(), record.end());
			}
			if (!slice.empty()) pack();
			// A record of no bytes ends them.
			slice.assign(4, 0);
			pack();
			System::PrintDiagnosticLine("[net-match] held client: its record of the lost host's round goes to the successor: frames=" + std::to_string(records.size()) +
			                            " through=" + std::to_string(m_CommittedRing.Count() > 0 ? m_CommittedRing.LastFrame() : 0) + " chunks=" + std::to_string(m_HeldRecordPackets.size()));
		}
		while (!m_HeldRecordPackets.empty() && m_MigratedTransport->Send(link, NetTransportLane::ControlReliable, m_HeldRecordPackets.front(), nullptr)) m_HeldRecordPackets.pop_front();
	}

	bool NetMatchService::TakeHeldRecordLocked(const NetTransportEvent& event) {
		if (event.type != NetTransportEventType::PacketReceived || NetLockstepCodec::LooksLikePacket(event.bytes)) return false;
		const NetLobbyDecodeResult decoded = NetLobbyProtocol::Decode(event.bytes);
		const NetLobbyStateChunk* chunk = decoded.ok ? std::get_if<NetLobbyStateChunk>(&decoded.message.payload) : nullptr;
		uint64_t round = 0;
		if (!chunk || chunk->transferId != c_NetWorldTailTransferId || !ParseWorldTailChunkRound(*chunk, round)) return false;
		// A record of another round is no part of this one.
		if (round != m_WorldCatchUp.roundId) return true;
		if (chunk->bytes.size() == c_NetWorldTailRoundBytes + 4 && std::all_of(chunk->bytes.begin() + c_NetWorldTailRoundBytes, chunk->bytes.end(), [](uint8_t byte) { return byte == 0; })) {
			m_HeldRecordsEnded.insert(event.peerId);
			System::PrintDiagnosticLine("[net-match] held client: a held seat's record of the lost host's round arrived connection=" + std::to_string(event.peerId) + " taken=" +
			                            std::to_string(m_HeldRecordFramesTaken) + " through=" + std::to_string(m_HeldRecordThrough) + " applied=" + std::to_string(ScenarioRunner::WorldCatchUpAppliedThrough()));
			return true;
		}
		std::vector<NetLockstepFrame> frames;
		for (size_t offset = c_NetWorldTailRoundBytes; offset + 4 <= chunk->bytes.size();) {
			const uint32_t size = static_cast<uint32_t>(chunk->bytes[offset]) | (static_cast<uint32_t>(chunk->bytes[offset + 1]) << 8) |
			                      (static_cast<uint32_t>(chunk->bytes[offset + 2]) << 16) | (static_cast<uint32_t>(chunk->bytes[offset + 3]) << 24);
			offset += 4;
			if (size == 0 || chunk->bytes.size() - offset < size) break;
			NetLockstepFrame frame;
			const uint64_t target = DecodeCommittedJoinFrame(std::vector<uint8_t>(chunk->bytes.begin() + offset, chunk->bytes.begin() + offset + size), frame) && frame.roundId == round ? frame.targetFrame : 0;
			offset += size;
			if (target == 0) break;
			m_HeldRecordThrough = std::max(m_HeldRecordThrough, target);
			if (target > ScenarioRunner::WorldCatchUpAppliedThrough() && !ScenarioRunner::WorldCatchUpHasFrame(target) &&
			    std::none_of(frames.begin(), frames.end(), [target](const NetLockstepFrame& held) { return held.targetFrame == target; })) frames.push_back(std::move(frame));
		}
		m_HeldRecordFramesTaken += frames.size();
		if (!frames.empty()) ScenarioRunner::AppendWorldCatchUp(std::move(frames));
		return true;
	}

	bool NetMatchService::HostHeldMatchLocked() {
		// Another held seat may have replayed more of the lost host's frames than this one: they are the round's own, so this seat replays
		// them before its round opens and that seat joins it in place. Every seat is held meanwhile, so nobody waits on a round.
		const Activity* activity = g_ActivityMan.GetActivity();
		if (m_HeldGatherSinceMs == 0) {
			if (activity && activity->IsOver()) {
				ScenarioRunner::SetControllerReplayError("MatchOver:the match ended while this seat was rejoining");
				return true;
			}
			m_HeldGatherExpected = m_HeldListener ? HeldSurvivorsLocked().size() : 0;
			if (m_HeldGatherExpected == 0) return HostAloneFromOwnStateLocked();
			m_HeldGatherSinceMs = SteadyNowMs();
			System::PrintDiagnosticLine("[net-match] held client: it hosts once the held seats' records are in: expected=" + std::to_string(m_HeldGatherExpected) + " arrived=" +
			                            std::to_string(m_HeldRecordsEnded.size()) + " applied=" + std::to_string(ScenarioRunner::WorldCatchUpAppliedThrough()));
			return true;
		}
		const uint64_t applied = ScenarioRunner::WorldCatchUpAppliedThrough();
		const bool replayed = !ScenarioRunner::WorldCatchUpHasFrame(applied + 1);
		const uint64_t waited = SteadyNowMs() - m_HeldGatherSinceMs;
		if ((m_HeldRecordsEnded.size() < m_HeldGatherExpected || !replayed) && waited < c_HeldGatherMs) return true;
		System::PrintDiagnosticLine("[net-match] held client: hosts after the held seats' records: arrived=" + std::to_string(m_HeldRecordsEnded.size()) + " of " +
		                            std::to_string(m_HeldGatherExpected) + " taken=" + std::to_string(m_HeldRecordFramesTaken) + " applied=" + std::to_string(applied) +
		                            " waited=" + std::to_string(waited) + "ms");
		m_HeldGatherSinceMs = 0;
		// Those frames may be the ones that ended the round by its own rules: then there is a result, not a match to host.
		if (activity && activity->IsOver()) {
			ScenarioRunner::SetControllerReplayError("MatchOver:the match ended while this seat was rejoining");
			return true;
		}
		return HostAloneFromOwnStateLocked();
	}

	bool NetMatchService::DriveInPlaceMoveLocked(uint64_t nowMs) {
		if (m_InPlaceMoveHost == 0) return true;
		m_Session->Tick(AdmissionNowMs());
		SendHeldRecordLocked();
		if (m_Session->IsReady()) {
			const std::vector<NetSessionPeerInfo> ready = m_Session->GetReadyPeers();
			const NetPeerId hostLink = ready.empty() ? c_InvalidNetPeerId : ready.front().transportPeerId;
			if (BindClientLobbyToHostLocked(*m_MigratedTransport, m_InPlaceMoveHost, hostLink, m_WorldCatchUp.appliedThrough + 1)) {
				// The successor hosts the match now: a later rejoin of this seat goes to it.
				if (m_LastJoinRoute) {
					m_LastJoinRoute->address = m_InPlaceMoveAddress.substr(0, m_InPlaceMoveAddress.rfind(':'));
					m_LastJoinRoute->port = static_cast<uint16_t>(std::stoul(m_InPlaceMoveAddress.substr(m_InPlaceMoveAddress.rfind(':') + 1)));
				}
				(void)m_Runner->GetLobbySession().SendPayload(MakeJoinerCatchUpReport(), nullptr);
				m_InPlaceAskedMs = m_InPlaceHeardMs = SteadyNowMs();
				m_InPlaceProgressApplied = m_WorldCatchUp.appliedThrough;
				System::PrintDiagnosticLine("[net-match] held client: the successor peer=" + std::to_string(m_InPlaceMoveHost) + " admitted the seat; its tail is asked from " +
				                            std::to_string(m_WorldCatchUp.appliedThrough) + " after " + std::to_string(SteadyNowMs() - m_InPlaceMoveSinceMs) + "ms clock=" + std::to_string(SteadyNowMs()));
				m_InPlaceMoveHost = 0;
				m_InPlaceTicketHost.clear();
				CloseHeldListenerLocked();
				return true;
			}
		}
		const bool failed = m_Session->IsFailed() || m_Session->GetState() == NetSessionState::Closed || m_Session->GetState() == NetSessionState::Rejected;
		if (!failed && SteadyNowMs() - m_InPlaceMoveSinceMs < c_InPlaceMoveBudgetMs) return true;
		System::PrintDiagnosticLine("[net-match] held client: the successor at " + m_InPlaceMoveAddress + " did not admit the seat: " +
		                            (failed ? m_Session->BuildRejectText() : std::string("no answer in time")));
		return DialNextInPlaceRouteLocked(nowMs);
	}

	bool NetMatchService::CrossReplayHandoverLocked(std::string* error) {
		const NetWorldHandover& handover = *m_WorldCatchUp.handover;
		const NetLockstepConfig& replayed = m_CatchUpCoordinator->GetConfig();
		NetLockstepConfig config;
		config.sessionId = replayed.sessionId;
		config.roundId = replayed.roundId;
		config.matchConfig = replayed.matchConfig;
		config.peerCount = replayed.peerCount; config.localPeerId = replayed.localPeerId;
		config.authorityPeerId = handover.authorityPeerId;
		config.startFrame = handover.frame;
		config.migrationGeneration = handover.generation;
		config.migrationKey = replayed.migrationKey;
		config.migrationTransportFactory = replayed.migrationTransportFactory;
		config.migrationIceDial = replayed.migrationIceDial;
		config.migrationIceHost = replayed.migrationIceHost;
		config.originalRoundConfigHash = replayed.originalRoundConfigHash;
		config.simTickMs = replayed.simTickMs;
		config.initialPeerLeaves = m_CatchUpCoordinator->GetPeerLeaveFrames();
		for (uint8_t peer = 1; peer <= config.peerCount && peer <= 8; ++peer)
			if ((handover.departedMask & (1U << (peer - 1))) != 0) config.initialPeerLeaves.emplace(peer, handover.frame);
		config.initialSeatHolds = m_CatchUpCoordinator->HeldTransactions();
		// A held seat that took the round over is its hub: under the AI until the return it commits, never gone.
		if (const auto left = config.initialPeerLeaves.find(handover.authorityPeerId); left != config.initialPeerLeaves.end() && left->second < handover.frame)
			config.initialPeerLeaves.erase(left);
		config.initialDelayChanges = m_CatchUpCoordinator->GetDelayChanges();
		auto transport = std::make_unique<LoopbackTransport>();
		auto replay = std::make_unique<NetLockstepCoordinator>();
		if (handover.authorityPeerId == 0 || handover.authorityPeerId > config.peerCount || !replay->StartReplay(*transport, config, error)) {
			if (error && error->empty()) *error = "the handover names no authority this round has";
			return false;
		}
		// The replay goes on under the new authority from the state it reached: what the switch clears, it takes back.
		NetResyncState committed;
		if (!ScenarioRunner::CaptureNetResyncState(handover.frame - 1, committed, error, false)) return false;
		committed.pendingInputs.clear(); committed.pendingCommands.clear(); committed.pendingPlayerBindings.clear(); committed.admittedReseats.clear();
		const auto pause = ScenarioRunner::CaptureLockstepPauseState();
		ScenarioRunner::SetLockstepCoordinator(replay.get(), true);
		if (!ScenarioRunner::RestoreCommittedCatchUpState(committed, error) || !ScenarioRunner::RestoreLockstepPauseState(pause, committed.savedTick)) return false;
		m_CatchUpCoordinator = std::move(replay);
		m_CatchUpTransport = std::move(transport);
		m_WorldCatchUp.authorityPeerId = handover.authorityPeerId;
		m_WorldCatchUp.authorityGeneration = handover.generation;
		m_WorldCatchUp.handoverCrossed = true;
		ScenarioRunner::SetWorldCatchUpFence(0);
		std::ostringstream line;
		line << "[net-match] held client: the replay crosses the handover at " << handover.frame << " under peer=" << static_cast<int>(handover.authorityPeerId)
		     << " generation=" << handover.generation << " departed_mask=" << static_cast<int>(handover.departedMask);
		System::PrintDiagnosticLine(line.str());
		return true;
	}

	void NetMatchService::OpenInPlaceRejoinLocked(const NetLobbySession::WorldJoinReport& report, const std::vector<NetSessionPeerInfo>& readyPeers, uint64_t nowMs) {
		const uint8_t member = report.fromPeer;
		// A seat whose return is agreed is on its way back: its last reports before the reclaim frame are progress, not a request.
		if (!m_Coordinator || !m_Session || !m_Runner || member == 0 || member == m_Coordinator->GetHostPeerId() || !m_Coordinator->HasHeldAISeat(member) ||
		    m_Coordinator->HasAgreedSeatReclaim(member)) return;
		const auto link = std::find_if(readyPeers.begin(), readyPeers.end(), [&](const NetSessionPeerInfo& peer) { return peer.assignedPeerId + 1 == member; });
		if (link == readyPeers.end() || m_Coordinator->UsesTransportPeer(link->transportPeerId)) return;
		const NetPeerId connection = link->transportPeerId;
		const auto heldSeats = m_Coordinator->HeldTransactions();
		const auto hold = heldSeats.find(member);
		// A return already on its way, on this connection or a relaunched one, owns the seat's catch-up; one whose reclaim frame the
		// seat was held again at or after is over, and this report opens the next.
		std::vector<NetPeerId> overtaken;
		for (const NetWorldJoinSession& session: m_WorldJoin.Sessions()) {
			if (session.phase == NetWorldJoinPhase::Active || (session.connection != connection && session.assignedPeerId != member)) continue;
			// A returner that reports the state it holds needs no image, so one not yet sent to it is dropped.
			const bool unsentImage = session.connection == connection && session.phase == NetWorldJoinPhase::SnapshotTransfer && !session.transferStarted;
			if (!unsentImage && (session.connection != connection || session.activationTick == 0 || hold == heldSeats.end() ||
			                     hold->second.cutoffFrame < session.activationTick)) return;
			overtaken.push_back(session.connection);
		}
		for (const NetPeerId stale: overtaken) {
			m_PrivateActivations.erase(stale);
			m_WorldJoin.CancelJoin(stale, "the seat was held again after its return");
			System::PrintDiagnosticLine("[net-match] held seat peer=" + std::to_string(member) + " was held again after its return; its next catch-up replaces the last");
		}
		const std::optional<uint16_t> seat = m_ReconnectHost.StableSeatOfConnection(connection);
		NetPeerId holder = c_InvalidNetPeerId; uint32_t generation = 0, incarnation = 0;
		if (hold == heldSeats.end() || !seat || !m_ReconnectHost.GetSeatHolder(*seat, holder, generation, incarnation) || holder != connection) return;
		const uint64_t heldThrough = report.value;
		std::string error;
		// A seat held before this host took the round over replayed the lost host's frames, which are the round's own up to the handover.
		const bool heldBeforeHandover = m_HandoverFrame != 0 && hold != heldSeats.end() && hold->second.cutoffFrame < m_HandoverFrame;
		if (heldBeforeHandover && heldThrough >= m_HandoverFrame) error = "its state ran past the handover at " + std::to_string(m_HandoverFrame);
		else if (!heldBeforeHandover && heldThrough >= hold->second.cutoffFrame) error = "its state ran past the hold at " + std::to_string(hold->second.cutoffFrame);
		// The connection's last return is done: its reclaim and its reasons belong to that return, not to this one.
		if (m_WorldJoin.FindSession(connection)) m_WorldJoin.CancelJoin(connection, "the seat was held again");
		m_PrivateActivations.erase(connection);
		m_PrivateTransferHeldReasons.erase(connection);
		// A return a lost host agreed and this round passed still spent its incarnation on every peer.
		const auto& known = m_Coordinator->GetConfig().peerIncarnations;
		const uint32_t returning = std::max(hold->second.seatIncarnation, known.contains(member) ? known.at(member) : 0U) + 1;
		if (error.empty() && m_WorldJoin.BeginInPlaceRejoin(connection, *seat, member, returning, link->displayName, nowMs, heldThrough, &error)) {
			m_InPlaceIncarnationBumps[member] = returning > incarnation ? returning - incarnation : 0;
			m_Coordinator->NoteInPlaceReturn(member);
			(void)m_Runner->GetLobbySession().BindWorldTransferRemote(member, connection, nullptr);
			// A state from before the handover replays across it, a survivor's included: the frame the lost host left at is in no command.
			const bool crossesHandover = m_HandoverFrame != 0 && heldThrough < m_HandoverFrame;
			if (crossesHandover) {
				// Ahead of the tail: where it changed hands, so the returner replays each side under the authority that committed it.
				NetWorldHandover handover;
				handover.frame = m_HandoverFrame;
				handover.generation = m_Coordinator->GetConfig().migrationGeneration;
				handover.authorityPeerId = m_Coordinator->GetHostPeerId();
				for (const auto& [peer, frame]: m_Coordinator->GetPeerLeaveFrames())
					if (frame == m_HandoverFrame && peer >= 1 && peer <= 8) handover.departedMask |= static_cast<uint8_t>(1U << (peer - 1));
				(void)m_Runner->GetLobbySession().SendPayloadTo(member, MakeWorldJoinHandoverReport(handover), nullptr);
			}
			if (const NetWorldJoinSession* opened = m_WorldJoin.FindSession(connection)) SendWorldJoinTailTo(m_Runner->GetLobbySession(), m_WorldJoin, *opened);
			std::ostringstream line;
			line << "[net-match] held seat catches up in place peer=" << static_cast<int>(member) << " from=" << heldThrough << " hold=" << hold->second.cutoffFrame
			     << " incarnation=" << returning << " horizon=" << m_Coordinator->GetStats().nextFrame;
			if (crossesHandover) line << " handover=" << m_HandoverFrame << " tail_first=" << m_WorldJoin.Tail().FirstServableFrame();
			System::PrintDiagnosticLine(line.str());
			return;
		}
		// No tail reaches its state: it comes back through the image on a new connection.
		System::PrintDiagnosticLine("[net-match] held seat peer=" + std::to_string(member) + " cannot catch up in place: " + error + " (sessions=" + std::to_string(m_WorldJoin.Sessions().size()) + ")");
		m_Session->DisconnectReadyPeer(connection, NetRejectReason::HostNotAccepting, c_ImageRejoinDetail);
	}

	bool NetMatchService::InPlaceLiveRoundLocked(NetLockstepConfig& live) {
		live = m_CatchUpCoordinator->GetConfig();
		live.roundId = m_WorldCatchUp.roundId; live.originalRoundConfigHash = m_WorldCatchUp.roundConfigHash;
		live.initialSeatHolds = m_CatchUpCoordinator->HeldTransactions();
		live.initialPeerLeaves = m_CatchUpCoordinator->GetPeerLeaveFrames();
		live.seatStateThroughFrame = m_WorldCatchUp.appliedThrough;
		std::vector<NetLockstepTiming> returns;
		std::map<uint8_t, std::map<uint64_t, uint16_t>> earlierDelays;
		for (const auto& event: m_CatchUpWirePackets) {
			if (event.bytes.size() < NetLockstepCodec::c_HeaderBytes || event.bytes[8] != static_cast<uint8_t>(NetLockstepPacketType::Timing)) continue;
			const auto decoded = NetLockstepCodec::Decode(event.bytes);
			if (!decoded.ok) continue;
			const auto* decision = std::get_if<NetLockstepTiming>(&decoded.packet.payload);
			if (!decision || decision->senderPeerId != m_CatchUpCoordinator->GetHostPeerId() || decision->sessionId != live.sessionId || decision->roundId != live.roundId || decision->authorityGeneration != live.migrationGeneration || decision->peerId > live.peerCount) continue;
			if (decision->action == NetTimingAction::Delay && decision->phase == NetTimingPhase::Commit && decision->applyFrame >= m_WorldCatchUp.activationTick)
				live.initialDelayChanges[decision->peerId][decision->applyFrame] = decision->delayFrames;
			else if (decision->action == NetTimingAction::Delay && decision->phase == NetTimingPhase::Commit)
				earlierDelays[decision->peerId][decision->applyFrame] = decision->delayFrames;
			if (decision->phase == NetTimingPhase::ReclaimAtFrame && decision->peerId == m_LocalPeerId && decision->applyFrame == m_WorldCatchUp.activationTick)
				live.initialSeatReclaims[m_LocalPeerId] = {m_LocalPeerId, decision->authorityGeneration, decision->revision,
				    decision->seatIncarnations[m_LocalPeerId - 1], decision->applyFrame, decision->delayFrames, decision->neutralThroughFrame};
			else if (decision->phase == NetTimingPhase::ReclaimAtFrame) returns.push_back(*decision);
		}
		NetLockstepCoordinator::AdoptReturnsBefore(live, returns, m_WorldCatchUp.activationTick);
		// Returns the held round or the replay agreed before this one, whose neutral gaps still run at the activation: no packet of
		// the catch-up carries them, and their seats' frames in the gap are never sent.
		std::map<uint8_t, NetGameSeatReclaim> agreed;
		if (m_Coordinator) agreed = m_Coordinator->ReclaimTransactions();
		for (const auto& [peer, reclaim]: m_CatchUpCoordinator->ReclaimTransactions())
			if (NetGameSeatReclaim& kept = agreed[peer]; kept.peerId == 0 || kept.eventSequence < reclaim.eventSequence) kept = reclaim;
		NetLockstepCoordinator::AdoptOpenReturns(live, agreed, m_WorldCatchUp.activationTick);
		if (live.matchConfig.peerInputDelayFrames.empty()) live.matchConfig.peerInputDelayFrames.resize(live.peerCount, live.matchConfig.inputDelayFrames);
		for (const auto& [peer, changes]: live.initialDelayChanges) {
			const auto at = changes.upper_bound(m_WorldCatchUp.activationTick);
			if (peer > 0 && peer <= live.peerCount && at != changes.begin()) live.matchConfig.peerInputDelayFrames[peer - 1] = std::prev(at)->second;
		}
		// The delay changes the round committed in the tail this seat has just replayed stay changes: every peer's
		// start still matches its opening delay, and each produces on the delay in force at the activation.
		for (const auto& [peer, changes]: m_CatchUpCoordinator->GetDelayChanges()) {
			const auto at = changes.upper_bound(m_WorldCatchUp.activationTick);
			if (peer > 0 && peer <= live.peerCount && at != changes.begin()) live.initialDelayChanges[peer].emplace(std::prev(at)->first, std::prev(at)->second);
		}
		// So do those committed on the wire while the replay had not reached them.
		for (const auto& [peer, changes]: earlierDelays) {
			const auto at = changes.upper_bound(m_WorldCatchUp.activationTick);
			if (peer > 0 && peer <= live.peerCount && at != changes.begin()) live.initialDelayChanges[peer].emplace(std::prev(at)->first, std::prev(at)->second);
		}
		return live.initialSeatReclaims.contains(m_LocalPeerId);
	}

	bool NetMatchService::TakeCatchUpRoundEndLocked(NetLobbySession& lobby, uint64_t* refusal) {
		std::optional<uint64_t> roundEnded;
		StepWorldJoinCatchUpClient(lobby, m_WorldCatchUp, refusal, &roundEnded);
		if (!roundEnded) return false;
		// The round ended while this seat caught up: the host's end record is its result, and the lobby that follows is the rematch's.
		m_RoundEndRecord = *roundEnded;
		m_ReceivedEndWinner = RoundEndedWinnerTeam(*roundEnded);
		m_HostGoodbyeSeen = true;
		m_CompletedRoundFinalFrame = RoundEndedFinalFrame(*roundEnded);
		for (const NetTransportEvent& event: lobby.TakeEventsAfterRoundEnded()) QueueLobbyEvent(event);
		System::PrintDiagnosticLine("[net-match] end record received final=" + std::to_string(m_CompletedRoundFinalFrame) + " winner_team=" +
		                            std::to_string(*m_ReceivedEndWinner) + " local_team=" + std::to_string(m_LocalTeam) + " in_place=1");
		ScenarioRunner::SetControllerReplayError("MatchOver: the round ended while this seat caught up");
		// The round this catch-up replayed is over: its record is taken once, and no later drive steps this catch-up again.
		m_WorldCatchUp.active = false;
		m_WorldCatchUp.endRecord.reset();
		return true;
	}

	void NetMatchService::DriveWorldJoinClient(uint64_t nowMs) {
		// This runs under m_Mutex and hands lobby messages to the session below, so the service calls
		// those messages make have to take the locked path.
		const LockedLobbyDrive lockedDrive(*this);
		// A member catching up privately is replaying on this thread and the round is not feeding its
		// session: that silence is its own, not the host's. The windows stay open for as long as the
		// catch-up runs; a host that really goes away still arrives as a transport close below.
		if (NetSession* live = LiveSessionLocked()) live->SetSilenceSuspended(!m_IsHost && m_WorldCatchUp.active);
		// A seat back in the round, or no longer catching up in place, listens for nobody.
		if (m_HeldListener && (m_IsHost || !m_WorldCatchUp.active || !m_InPlaceCatchUp)) CloseHeldListenerLocked();
		if (m_IsHost || !m_WorldCatchUp.active || !m_Runner) return;
		NetLobbySession& lobby = m_Runner->GetLobbySession();
		INetTransport* wire = ActiveWireLocked();
		// A coordinator already handshaking for the activation polls the same wire; the tail and the reports it read go on here.
		std::vector<NetTransportEvent> polledLobby;
		polledLobby.swap(m_PendingLobbyEvents);
		m_PendingLobbyBytes = 0;
		m_PendingLobbyOverflow = false;
		NoteDroppedLobbyEvents(polledLobby.size());
		for (const NetTransportEvent& event: polledLobby) lobby.HandleTransportEvent(event, nowMs);
		if (!polledLobby.empty()) m_InPlaceHeardMs = SteadyNowMs();
		if (wire) {
			for (const NetTransportEvent& event: wire->PollEvents()) {
				if (event.type == NetTransportEventType::PacketReceived) m_InPlaceHeardMs = SteadyNowMs();
				if (event.type == NetTransportEventType::PacketReceived && NetLobbyProtocol::Decode(event.bytes).ok) {
					lobby.HandleTransportEvent(event, nowMs);
				} else if (event.type == NetTransportEventType::PacketReceived && NetLockstepCodec::LooksLikePacket(event.bytes)) {
					// The host's acks only say it is alive, which the stamp above has taken; they name the frames of the round this seat left.
					if (event.bytes.size() > 8 && event.bytes[8] == static_cast<uint8_t>(NetLockstepPacketType::Ack)) continue;
					if (m_CatchUpWireBytes + event.bytes.size() > 16ULL * 1024 * 1024) {
						m_PrivateJoinError = "private catch-up wire backlog exceeded its bound";
						m_State = NetMatchServiceState::Failed; m_ErrorText = m_PrivateJoinError; return;
					}
					m_CatchUpWireBytes += event.bytes.size(); m_CatchUpWirePackets.push_back(event);
				} else if (m_Session) m_Session->InjectEvent(event, nowMs);
			}
		}
		// The first survivor listens while it dials: a held seat's own session at its listener proves that seat reads it as the host.
		if (m_HeldListener && m_InPlaceCatchUp) {
			for (NetTransportEvent& event: m_HeldListener->PollEvents()) {
				if (TakeHeldRecordLocked(event)) continue;
				m_HeldDialSeen = m_HeldDialSeen || (event.type == NetTransportEventType::PacketReceived && !NetLockstepCodec::LooksLikePacket(event.bytes) && !NetLobbyProtocol::Decode(event.bytes).ok);
				m_HeldListenerEvents.push_back(std::move(event));
			}
			bool proven = m_HeldDialSeen;
			// A live host acks each held seat every tick: one this seat has heard lately is alive whatever the dialer's own link said.
			const uint64_t silentMs = SteadyNowMs() > m_InPlaceHeardMs ? SteadyNowMs() - m_InPlaceHeardMs : 0;
			if (proven && silentMs <= c_HeldDialProofSilenceMs) {
				if (!m_HeldDialNoted) System::PrintDiagnosticLine("[net-match] held client: a held seat dialed this one's listener while its host spoke " + std::to_string(silentMs) + "ms ago; it waits");
				m_HeldDialNoted = true;
				proven = false;
			}
			if (proven && m_HeldGatherSinceMs == 0) {
				System::PrintDiagnosticLine("[net-match] held client: a held seat dialed this one's listener and its host has been silent " + std::to_string(silentMs) + "ms; it hosts the match");
				m_StatusText = "Host lost - arranging handover";
				m_HeldDialNoted = false;
				m_InPlaceMoveHost = 0;
				m_InPlaceRoutes.clear();
				if (m_MigratedTransport) m_MigratedTransport->Stop();
				if (HostHeldMatchLocked()) return;
			}
		}
		// The first survivor hosts once the other held seats' records of the lost host's round are in.
		if (m_HeldGatherSinceMs != 0) {
			if (!HostHeldMatchLocked()) {
				m_InPlaceCatchUp = false;
				ScenarioRunner::SetControllerReplayError("PeerHeld:Held - AI in control - reconnecting the private catch-up link");
			}
			return;
		}
		if (m_InPlaceMoveHost != 0) {
			if (!DriveInPlaceMoveLocked(nowMs)) {
				m_InPlaceCatchUp = false;
				ScenarioRunner::SetControllerReplayError("PeerHeld:Held - AI in control - no successor took the catch-up; rejoining");
				return;
			}
			if (m_InPlaceMoveHost != 0) return;
		}
		if (m_Session && !lobby.GetRoundEndedRecord() && (m_Session->IsFailed() || m_Session->GetState() == NetSessionState::Closed || m_Session->GetState() == NetSessionState::Rejected)) {
			// A host that reached its own last tick says goodbye; a seat that is still catching up must read that
			// as the round ending, not as a link to rejoin, or it spends its rejoin on a match that is over.
			if (NoteHostGoodbyeLocked(m_Session.get()) || !g_ActivityMan.ActivityRunning() ||
			    (g_ActivityMan.GetActivity() && g_ActivityMan.GetActivity()->IsOver())) {
				ScenarioRunner::SetControllerReplayError("MatchOver:the match ended while this seat was rejoining");
				return;
			}
			// A host that is gone hands the catch-up to its successor, and the seat keeps the world it holds; with only held seats left,
			// the first of them hosts the match from its own committed state.
			const std::string rejectText = m_Session->BuildRejectText();
			const bool hostGone = HeldSeatHostIsGone(true, m_Session->HasReject(), m_Session->GetRejectReason(), rejectText.find(c_OwnTransportStopDetail) != std::string::npos,
			                                        rejectText.find(c_ImageRejoinDetail) != std::string::npos, 0, 0);
			if (m_InPlaceCatchUp && hostGone) m_StatusText = "Host lost - arranging handover";
			if (m_InPlaceCatchUp && hostGone && HeldSeatHostsLocked() && HostHeldMatchLocked()) return;
			if (m_InPlaceCatchUp && hostGone && BeginInPlaceMoveLocked(nowMs)) return;
			ScenarioRunner::SetControllerReplayError(!m_HeldUnreachableText.empty() ? m_HeldUnreachableText : m_WorldCatchUp.privateMatch
			    ? "PeerHeld:Held - AI in control - reconnecting the private catch-up link"
			    : rejectText.find(c_HistoryPassedDetail) != std::string::npos ? "PeerLeft:The world moved on past your catch-up - join again"
			    : "PeerLeft:The host connection was lost while joining the world");
			return;
		}
		uint64_t refusal = 0;
		const uint64_t priorActivation = m_WorldCatchUp.activationTick;
		if (TakeCatchUpRoundEndLocked(lobby, &refusal)) return;
		if (m_WorldCatchUp.endRecord) return;
		// A different frame is the host's next return for this seat: it held the seat again after the last one, and the start that
		// return began is over whether or not this peer saw the hold.
		if (m_WorldCatchUp.privateMatch && priorActivation != 0 && m_WorldCatchUp.activationTick != 0 && m_WorldCatchUp.activationTick != priorActivation) {
			System::PrintDiagnosticLine("[net-match] held client: the host moved this seat's return from " + std::to_string(priorActivation) + " to " +
			                            std::to_string(m_WorldCatchUp.activationTick) + " at replayed frame " + std::to_string(m_WorldCatchUp.appliedThrough));
			DropReturnStartLocked("the host moved this seat's return");
		}
		if (refusal != 0) {
			m_State = NetMatchServiceState::Failed; m_ErrorText = NetWorldJoinRefusalText(refusal); return;
		}
		// The host's link is its liveness, never its tail: a host that closes the link or says nothing at all past the host-loss bound
		// is gone, and the seat rejoins the next host, or hosts the match itself when only held seats are left.
		if (m_InPlaceCatchUp) {
			const uint64_t steadyMs = SteadyNowMs();
			if (m_WorldCatchUp.appliedThrough >= m_InPlaceProgressLogged + 60) {
				m_InPlaceProgressLogged = m_WorldCatchUp.appliedThrough;
				System::PrintDiagnosticLine("[net-match] held client catch-up applied=" + std::to_string(m_WorldCatchUp.appliedThrough) + " activation=" +
				                            std::to_string(m_WorldCatchUp.activationTick) + " work_ticks=" + std::to_string(ScenarioRunner::WorldCatchUpWorkTicks()) +
				                            " wire_packets=" + std::to_string(m_CatchUpWirePackets.size()) + " datagrams=" + std::to_string(m_WorldCatchUp.tailDatagrams) +
				                            " kept=" + std::to_string(m_WorldCatchUp.tailFramesKept) + " repeated=" + std::to_string(m_WorldCatchUp.tailFramesRepeated) +
				                            " buffered=" + std::to_string(ScenarioRunner::WorldCatchUpHasFrame(m_WorldCatchUp.appliedThrough + 1)));
			}
			uint64_t hostRttMs = 0;
			if (INetTransport* wire = ActiveWireLocked(); wire && m_Session) hostRttMs = wire->GetPeerPingMs(m_Session->GetRemoteTransportPeerId());
			if (steadyMs > m_InPlaceHeardMs && HeldSeatHostIsGone(false, false, NetRejectReason::InternalError, false, false, steadyMs - m_InPlaceHeardMs, hostRttMs)) {
				System::PrintDiagnosticLine("[net-match] held client: the host sent nothing for " + std::to_string(steadyMs - m_InPlaceHeardMs) + "ms at frame " +
				                            std::to_string(m_WorldCatchUp.appliedThrough));
				// The committed frames it replayed already ended the round by its own rules: the seat has the result, not a match to carry on.
				if (const Activity* activity = g_ActivityMan.GetActivity(); activity && activity->IsOver()) {
					ScenarioRunner::SetControllerReplayError("MatchOver:the match ended while this seat was rejoining");
					return;
				}
				m_StatusText = "Host lost - arranging handover";
				if (HeldSeatHostsLocked() && HostHeldMatchLocked()) return;
				if (BeginInPlaceMoveLocked(nowMs)) return;
				m_InPlaceCatchUp = false;
				ScenarioRunner::SetControllerReplayError(!m_HeldUnreachableText.empty() ? m_HeldUnreachableText : "PeerHeld:Held - AI in control - the host sent no catch-up; rejoining");
				return;
			}
		}
		// The replay crosses the round's handover under the authority that committed each side of it.
		if (m_InPlaceCatchUp && m_WorldCatchUp.handover && !m_WorldCatchUp.handoverCrossed) {
			std::string error;
			if (m_WorldCatchUp.appliedThrough + 1 > m_WorldCatchUp.handover->frame) error = "its state ran past the handover at " + std::to_string(m_WorldCatchUp.handover->frame);
			else if (m_WorldCatchUp.appliedThrough + 1 == m_WorldCatchUp.handover->frame) (void)CrossReplayHandoverLocked(&error);
			if (!error.empty()) {
				System::PrintDiagnosticLine("[net-match] held client: the replay cannot cross the handover: " + error);
				m_InPlaceCatchUp = false;
				ScenarioRunner::SetControllerReplayError("PeerHeld:Held - AI in control - rejoining through the image");
				return;
			}
		}
		// The held seat asks for its tail until the first frame of it lands.
		if (m_InPlaceCatchUp && m_WorldCatchUp.appliedThrough == m_WorldCatchUp.snapshotTick && !ScenarioRunner::WorldCatchUpHasFrame(m_WorldCatchUp.snapshotTick + 1)) {
			const uint64_t steadyMs = SteadyNowMs();
			if (steadyMs - m_InPlaceAskedMs >= 500) {
				(void)lobby.SendPayload(MakeJoinerCatchUpReport(), nullptr);
				m_InPlaceAskedMs = steadyMs;
				std::ostringstream line;
				line << "[net-match] held client asks for its tail again from=" << m_WorldCatchUp.snapshotTick << " activation=" << m_WorldCatchUp.activationTick
				     << " wire_packets=" << m_CatchUpWirePackets.size() << " starting=" << m_Runner->IsWorldJoinLockstepStarting() << " waited_ms=" << (steadyMs - m_InPlaceSinceMs);
				System::PrintDiagnosticLine(line.str());
			}
		}
		// A host that holds this seat again has taken back the return it agreed: the catch-up carries on toward the next one,
		// on the round's own record of both, and the start that return began is dropped. The hold may reach this wire or the
		// round already started for the return, and that round may be running while the replay has not yet reached its frame.
		if (m_WorldCatchUp.privateMatch && m_WorldCatchUp.activationTick != 0 && m_CatchUpCoordinator && m_Coordinator) {
			const uint8_t localBit = static_cast<uint8_t>(1U << (m_LocalPeerId - 1));
			const bool heldAgain = m_Coordinator->HeldLocalSeatSince(m_WorldCatchUp.activationTick) ||
			                       std::any_of(m_CatchUpWirePackets.begin(), m_CatchUpWirePackets.end(), [&](const NetTransportEvent& event) {
				if (event.bytes.size() < NetLockstepCodec::c_HeaderBytes || event.bytes[8] != static_cast<uint8_t>(NetLockstepPacketType::Timing)) return false;
				const auto decoded = NetLockstepCodec::Decode(event.bytes);
				const auto* decision = decoded.ok ? std::get_if<NetLockstepTiming>(&decoded.packet.payload) : nullptr;
				const bool held = decision && decision->phase == NetTimingPhase::HoldAtFrame && (decision->heldPeers & localBit) != 0 && decision->applyFrame >= m_WorldCatchUp.activationTick &&
				       decision->senderPeerId == m_CatchUpCoordinator->GetHostPeerId() && decision->roundId == m_WorldCatchUp.roundId;
				if (held) System::PrintDiagnosticLine("[net-match] held client: the hold on the wire is revision=" + std::to_string(decision->revision) + " action=" +
				                                      std::to_string(static_cast<int>(decision->action)) + " at=" + std::to_string(decision->applyFrame) + " cutoff=" +
				                                      std::to_string(decision->cutoffFrame) + " incarnation=" + std::to_string(decision->seatIncarnations[m_LocalPeerId - 1]));
				return held;
			});
			if (heldAgain) {
				System::PrintDiagnosticLine("[net-match] held client: the host held this seat again before its return at " + std::to_string(m_WorldCatchUp.activationTick) +
				                            "; catching up on from " + std::to_string(m_WorldCatchUp.appliedThrough) + " (round " +
				                            (m_Coordinator->HeldLocalSeatSince(m_WorldCatchUp.activationTick) ? "held it: " + m_Coordinator->GetStats().timeoutReason : std::string("running; a hold on the wire")) + ")");
				DropReturnStartLocked("the host held this seat again before its return");
				m_WorldCatchUp.activationTick = 0;
				m_WorldCatchUp.activationCommitted = false;
				ScenarioRunner::SetWorldCatchUpActivation(0);
				// The next report asks for the tail from here, and the host answers it as a fresh in-place return.
				(void)lobby.SendPayload(MakeJoinerCatchUpReport(), nullptr);
			}
		}
		if ((m_WorldCatchUp.privateMatch || m_WorldCatchUp.activationCommitted) && m_WorldCatchUp.activationTick != 0 && m_Coordinator &&
		    !m_Coordinator->IsRunning() && m_Session && wire) {
			std::string error;
			if (m_WorldCatchUp.privateMatch && !m_Runner->IsWorldJoinLockstepStarting() && m_CatchUpCoordinator) {
				NetLockstepConfig live;
				if (!InPlaceLiveRoundLocked(live)) return;
				{
					std::ostringstream line;
					line << "[net-match] live round from the catch-up at " << m_WorldCatchUp.activationTick << ": held";
					for (const auto& [peer, hold]: live.initialSeatHolds) line << ' ' << static_cast<int>(peer) << '@' << hold.cutoffFrame;
					line << " left";
					for (const auto& [peer, frame]: live.initialPeerLeaves) line << ' ' << static_cast<int>(peer) << '@' << frame;
					line << " returns";
					for (const auto& [peer, reclaim]: live.initialSeatReclaims) line << ' ' << static_cast<int>(peer) << '@' << reclaim.activationFrame;
					line << " through=" << live.seatStateThroughFrame;
					System::PrintDiagnosticLine(line.str());
				}
				m_Runner->ConfigurePrivateJoin(live);
			}
			if (!m_WorldCatchUp.privateMatch && !m_Runner->IsWorldJoinLockstepStarting() && m_CatchUpCoordinator) {
				// The seats the replayed tail holds at the activation stay held in the joiner's live round: it never waits on their input.
				std::map<uint8_t, NetGameSeatHold> holds;
				for (const auto& [peer, hold]: m_CatchUpCoordinator->HeldTransactions())
					if (peer != m_LocalPeerId && hold.cutoffFrame <= m_WorldCatchUp.activationTick) holds[peer] = hold;
				std::map<uint8_t, uint64_t> leaves;
				for (const auto& [peer, frame]: m_CatchUpCoordinator->GetPeerLeaveFrames())
					if (peer != m_LocalPeerId && !holds.contains(peer) && frame <= m_WorldCatchUp.activationTick) leaves[peer] = frame;
				std::ostringstream line;
				line << "[net-match] world round from the catch-up at " << m_WorldCatchUp.activationTick << ": held";
				for (const auto& [peer, hold]: holds) line << ' ' << static_cast<int>(peer) << '@' << hold.cutoffFrame;
				line << " left";
				for (const auto& [peer, frame]: leaves) line << ' ' << static_cast<int>(peer) << '@' << frame;
				System::PrintDiagnosticLine(line.str());
				m_Runner->ConfigureWorldJoinSeats(std::move(holds), std::move(leaves));
			}
			if (!m_Runner->IsWorldJoinLockstepStarting())
				m_Runner->StartWorldJoinLockstep(*wire, *m_Session, *m_Coordinator, m_WorldCatchUp.activationTick, m_WorldCatchUp.appliedThrough, &error);
			else {
				for (auto it = m_CatchUpWirePackets.begin(); it != m_CatchUpWirePackets.end();) {
					if (it->bytes.size() >= NetLockstepCodec::c_HeaderBytes && (it->bytes[8] == static_cast<uint8_t>(NetLockstepPacketType::Start) || it->bytes[8] == static_cast<uint8_t>(NetLockstepPacketType::Timing))) {
						m_Coordinator->InjectEvent(*it, NetLockstepNowMs()); m_CatchUpWireBytes -= it->bytes.size(); it = m_CatchUpWirePackets.erase(it);
					} else ++it;
				}
				m_Runner->PumpWorldJoinLockstepStart(*m_Coordinator, m_WorldCatchUp.appliedThrough, &error);
			}
			if (!error.empty()) { m_PrivateJoinError = error; m_ErrorText = error; }
		}
		// The connection handshakes ahead of E; the simulation changes producer only after E-1.
		if (m_WorldCatchUp.activationTick != 0 && m_WorldCatchUp.appliedThrough + 1 < m_WorldCatchUp.activationTick) return;
		if (m_Coordinator && m_Coordinator->IsRunning() && m_WorldCatchUp.privateMatch) {
			// The switch is exact: the replay stopped on the frame before the return, and the live round starts on it.
			if (m_WorldCatchUp.activationTick != 0 && m_WorldCatchUp.appliedThrough + 1 != m_WorldCatchUp.activationTick) {
				ScenarioRunner::SetControllerReplayError("private catch-up replayed through " + std::to_string(m_WorldCatchUp.appliedThrough) + " past its return at " + std::to_string(m_WorldCatchUp.activationTick));
				return;
			}
			if (const uint64_t liveStart = m_Coordinator->GetConfig().startFrame; m_WorldCatchUp.activationTick != 0 && liveStart != m_WorldCatchUp.activationTick) {
				System::PrintDiagnosticLine("[net-match] held client: the live round starts at " + std::to_string(liveStart) + ", not at the return at " + std::to_string(m_WorldCatchUp.activationTick) + "; starting it again");
				DropReturnStartLocked("the live round was configured for another return");
				return;
			}
			const auto activationBegan = std::chrono::steady_clock::now();
			NetResyncState committed;
			std::string error;
			if (!ScenarioRunner::CaptureNetResyncState(m_WorldCatchUp.activationTick - 1, committed, &error, false)) { ScenarioRunner::SetControllerReplayError("private catch-up activation: " + error); return; }
			committed.pendingInputs.clear(); committed.pendingCommands.clear(); committed.pendingPlayerBindings.clear(); committed.admittedReseats.clear();
			const auto activationCaptured = std::chrono::steady_clock::now();
			const auto pause = ScenarioRunner::CaptureLockstepPauseState();
			if (m_CatchUpCoordinator) m_Coordinator->AdoptReplayedSeatTransitions(*m_CatchUpCoordinator, m_WorldCatchUp.activationTick - 1);
			ScenarioRunner::SetLockstepCoordinator(m_Coordinator.get(), true);
			if (!ScenarioRunner::RestoreCommittedCatchUpState(committed, &error) || !ScenarioRunner::RestoreLockstepPauseState(pause, committed.savedTick)) {
				ScenarioRunner::SetControllerReplayError("private catch-up activation: " + error); return;
			}
			if (g_ActivityMan.GetActivity() && m_ActivateCatchUpLocalSeat && !m_ActivateCatchUpLocalSeat(*g_ActivityMan.GetActivity())) {
				// A relaunched returner has no player state of its own; a seat held since its round began has no agreed binding either.
				const auto binding = committed.playerBindings.find(m_LocalPeerId);
				const auto roster = ScenarioRunner::GetLockstepMatchConfig();
				if (!Activity::RestoreReturningLocalSeat(*g_ActivityMan.GetActivity(), binding != committed.playerBindings.end() ? &binding->second.bindings : nullptr,
				                                         roster ? &*roster : nullptr, m_LocalPeerId)) {
					ScenarioRunner::SetControllerReplayError("private catch-up could not restore the local seat"); return;
				}
			}
			g_UInputMan.ClearMouseButtons();
			for (MovableObject* object: g_MovableMan.SnapshotKnownObjects()) if (auto* actor = dynamic_cast<Actor*>(object))
				actor->GetController()->ResetLocalInputState(actor->GetController()->GetInputMode());
			const auto activationLocal = std::chrono::steady_clock::now();
			for (const auto& event: m_CatchUpWirePackets) {
				if (event.bytes.size() > 17 && event.bytes[8] == static_cast<uint8_t>(NetLockstepPacketType::Frame)) {
					const size_t offset = event.bytes[17] == 0 ? 20 : 28;
					if (event.bytes.size() < offset + 8) continue;
					uint64_t first = 0; for (size_t i = 0; i < 8; ++i) first |= uint64_t(event.bytes[offset + i]) << (8 * i);
					if (first < m_WorldCatchUp.activationTick) continue;
				}
				m_Coordinator->InjectEvent(event, NetLockstepNowMs());
			}
			const auto activationWired = std::chrono::steady_clock::now();
			m_CatchUpWirePackets.clear(); m_CatchUpWireBytes = 0;
			m_CatchUpCoordinator.reset(); m_CatchUpTransport.reset(); m_ActivateCatchUpLocalSeat = {};
			// The tail replayed on this thread; the session read nothing while it ran, so neither the silence
			// windows nor the admission deadlines count it.
			m_AdmissionClock.NotePark(static_cast<uint64_t>(std::max(0.0,
			    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - activationBegan).count())), SteadyNowMs());
			NotePumpParkedLocked();
			SetRejoinPhaseLocked(NetSession::RejoinPhase::Active);
			if (NetSession* live = LiveSessionLocked()) live->TickKeepalive(AdmissionNowMs());
			// The host judges this seat's first input from the moment it knows the replay reached the frame before its return.
			(void)m_Runner->GetLobbySession().SendPayload(MakeJoinerCatchUpReport(), nullptr);
			const auto milliseconds = [](auto from, auto to) { return std::chrono::duration<double, std::milli>(to - from).count(); };
			{
				std::ostringstream line;
				line << "[net-match] activation work frame=" << m_WorldCatchUp.activationTick
				     << " capture_ms=" << milliseconds(activationBegan, activationCaptured) << " local_ms=" << milliseconds(activationCaptured, activationLocal)
				     << " wire_ms=" << milliseconds(activationLocal, activationWired) << " cleanup_ms=" << milliseconds(activationWired, std::chrono::steady_clock::now());
				System::PrintDiagnosticLine(line.str());
			}
			{
				std::ostringstream line;
				line << "[net-match] private catch-up complete frame=" << m_WorldCatchUp.activationTick << " in_place=" << m_InPlaceCatchUp
				     << " from=" << m_WorldCatchUp.snapshotTick << " clock=" << NetLockstepSharedClockMs();
				System::PrintDiagnosticLine(line.str());
			}
			m_InPlaceCatchUp = false;
			m_WorldCatchUp.handedToRound = true;
		}
		if (m_Coordinator && m_Coordinator->IsRunning() && !m_WorldCatchUp.privateMatch) {
			// The world's own coordinator takes the round from its replay at the activation, with the committed state it replayed to.
			if (m_CatchUpCoordinator) {
				NetResyncState committed;
				std::string error;
				if (!ScenarioRunner::CaptureNetResyncState(m_WorldCatchUp.activationTick - 1, committed, &error, false)) {
					ScenarioRunner::SetControllerReplayError("PeerLeft:world catch-up activation: " + error); return;
				}
				committed.pendingInputs.clear(); committed.pendingCommands.clear(); committed.pendingPlayerBindings.clear(); committed.admittedReseats.clear();
				committed.sessionId = m_Coordinator->GetConfig().sessionId;
				const auto pause = ScenarioRunner::CaptureLockstepPauseState();
				AdoptWorldReplaySeats(*m_Coordinator, *m_CatchUpCoordinator, m_WorldCatchUp.activationTick);
				ScenarioRunner::SetLockstepCoordinator(m_Coordinator.get(), true);
				if (!ScenarioRunner::RestoreCommittedCatchUpState(committed, &error) || !ScenarioRunner::RestoreLockstepPauseState(pause, committed.savedTick)) {
					ScenarioRunner::SetControllerReplayError("PeerLeft:world catch-up activation: " + error); return;
				}
				m_CatchUpCoordinator.reset(); m_CatchUpTransport.reset();
			}
			for (const auto& event: m_CatchUpWirePackets) {
				if (event.bytes.size() > 17 && event.bytes[8] == static_cast<uint8_t>(NetLockstepPacketType::Frame)) {
					const size_t offset = event.bytes[17] == 0 ? 20 : 28;
					if (event.bytes.size() < offset + 8) continue;
					uint64_t frame = 0; for (size_t i = 0; i < 8; ++i) frame |= uint64_t(event.bytes[offset + i]) << (8 * i);
					if (frame < m_WorldCatchUp.activationTick) continue;
				}
				m_Coordinator->InjectEvent(event, NetLockstepNowMs());
			}
			m_CatchUpWirePackets.clear(); m_CatchUpWireBytes = 0;
			// The world's tail is replayed; this seat plays live from here and its silence counts again.
			SetRejoinPhaseLocked(NetSession::RejoinPhase::Active);
			{
				std::ostringstream line;
				line << "[net-world] catch-up complete peer=" << static_cast<int>(m_Coordinator->GetConfig().localPeerId)
			          << " at=" << m_WorldCatchUp.activationTick << " input_horizon=" << m_Coordinator->GetStats().nextFrame;
				System::PrintDiagnosticLine(line.str());
			}
			// A watcher promoted into a seat plays it under the seat's id from here, and every later reading of this peer names that seat.
			if (const uint8_t live = m_Coordinator->GetConfig().localPeerId; live != 0 && live != m_LocalPeerId) {
				m_LocalPeerId = live;
				for (const NetMatchPlayerSlot& slot: m_Coordinator->GetConfig().matchConfig.players)
					if (slot.peerId == live) m_LocalTeam = slot.team;
				// The world it watched seated nobody on this machine: the seat's player takes it the way a returner does.
				if (Activity* activity = g_ActivityMan.GetActivity(); activity && !activity->AdoptNetLocalSeat(m_Coordinator->GetConfig().matchConfig, live)) {
					ScenarioRunner::SetControllerReplayError("PeerLeft:world catch-up activation: the promoted watcher could not take its seat"); return;
				}
				m_WorldWatcherSeatedAt = m_WorldCatchUp.activationTick;
			}
			m_WorldCatchUp.handedToRound = true;
		}
		if (m_Coordinator && m_Coordinator->IsRunning()) {
			m_Coordinator->DeferStopsToTickBoundary();
			m_AutosaveIdentity.roundId = m_Coordinator->GetRoundId();
			const auto& config = m_Coordinator->GetConfig();
			System::PrintDiagnosticLine(std::format("[net-lockstep] start round={} frame={} local_peer={} peers={} input_delay={}\n",
			    m_Coordinator->GetRoundId(), config.startFrame, config.localPeerId, config.peerCount, config.inputDelayFrames));
			// Starting the joined round handed its session traffic to the runner's start queue, which nothing reads after the start:
			// the session takes it back, the queued events first, or a returner never hears the roster, the chat or a kick again.
			AttachCoordinatorSessionSink();
		}
		ReleaseWorldCatchUpOnceRunning(m_Coordinator && m_Coordinator->IsRunning(), m_WorldCatchUp);
	}

	void NetMatchService::NoteRewindAnchor(const std::string& matchId, uint64_t tick, bool host, const AutosaveDescriptor* known) {
		if (matchId.empty() || tick == 0) return;
		std::string refusal;
		// The caller that already validated this checkpoint does not pay for a second read of it.
		const bool knownCheckpoint = known && known->matchId == matchId && known->savedTick == tick;
		const std::optional<AutosaveDescriptor> found = knownCheckpoint ? std::nullopt : AutosaveStore::Find(matchId, tick, &refusal);
		const AutosaveDescriptor* checkpoint = knownCheckpoint ? known : (found ? &*found : nullptr);
		const bool held = checkpoint != nullptr;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			// Retention keeps the agreed checkpoint whatever its age, so the rejoin still finds it.
			if (matchId == m_AutosaveMatchId) m_PinnedAutosave->Store(checkpoint ? checkpoint->roundId : m_AutosaveIdentity.roundId, tick);
			m_RewindAnchorMatchId = matchId;
			m_RewindAnchorTick = tick;
			m_RewindAnchorHeld = held;
		}
		System::PrintDiagnosticLine(std::format("[autosave] anchor {} match={} tick={} local={}\n", host ? "named" : "received",
		                         matchId, tick, held ? std::string("ok") : refusal));
	}

	NetMatchService::RewindAnchor NetMatchService::GetRewindAnchor() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return {m_RewindAnchorMatchId, m_RewindAnchorTick, m_RewindAnchorHeld};
	}

	void NetMatchService::ReportFakeLinkEffects() {
		int jitterMs = 0;
		float reorderPercent = 0, duplicatePercent = 0;
		GnsTransport::GetFakeLinkSettings(jitterMs, reorderPercent, duplicatePercent);
		if ((jitterMs <= 0 && reorderPercent <= 0 && duplicatePercent <= 0) || !m_Coordinator || !m_Coordinator->IsRunning()) return;
		NetFakeLinkEffects now;
		for (GnsTransport* transport: {m_Transport.get(), m_Mux ? m_Mux->IpGns() : nullptr, m_Mux ? m_Mux->P2PGns() : nullptr}) {
			if (!transport) continue;
			const NetFakeLinkEffects one = transport->GetFakeLinkEffects();
			now.jitterPackets += one.jitterPackets;
			now.reorderedPackets += one.reorderedPackets;
			now.duplicatedPackets += one.duplicatedPackets;
		}
		const uint64_t round = m_Coordinator->GetRoundId();
		if (round != m_FakeLinkRound) {
			m_FakeLinkRound = round;
			m_FakeLinkBaseline = now;
			m_FakeLinkReportedAtMs = SteadyNowMs();
			return;
		}
		if (SteadyNowMs() < m_FakeLinkReportedAtMs + 2000) return;
		m_FakeLinkReportedAtMs = SteadyNowMs();
		System::PrintDiagnosticLine("[net-fake-link] " + nlohmann::json{{"round", round}, {"peer", m_Coordinator->GetConfig().localPeerId}, {"jitter_ms", jitterMs},
		    {"reorder_percent", reorderPercent}, {"dup_percent", duplicatePercent}, {"jitter_packets", now.jitterPackets - m_FakeLinkBaseline.jitterPackets},
		    {"reordered_packets", now.reorderedPackets - m_FakeLinkBaseline.reorderedPackets}, {"duplicated_packets", now.duplicatedPackets - m_FakeLinkBaseline.duplicatedPackets}}.dump());
	}

	std::string NetMatchService::IdentityDigest(uint64_t id) {
		// A player's identity goes out as a digest of its stable id, never the id or its ticket.
		std::array<uint8_t, 8> bytes{};
		for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(id >> (8 * i));
		return System::Sha256Hex(bytes.data(), bytes.size());
	}

	uint64_t NetMatchService::GetLastRemovalBoundary() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_LastRemovalIssue.notice.boundaryFrame;
	}

	std::optional<NetMatchService::OwnRemoval> NetMatchService::GetOwnRemoval() const {
		NetLockstepPlane::Gap plane("own removal");
		std::lock_guard<std::mutex> lock(m_Mutex);
		const NetReconnectClient* client = m_Session ? m_Session->GetReconnectClient() : nullptr;
		if (client && client->WasRemoved()) return OwnRemoval{client->GetLastRejectReason(), client->GetRemovalBoundary()};
		// The round ends on the removal before its notice reaches the session; the round kept the notice's frame.
		if (m_IsHost || !m_Coordinator || m_Coordinator->GetRemovalBoundary() == 0 || !m_Session || !m_Session->HasReject()) return std::nullopt;
		return OwnRemoval{m_Session->GetRejectReason(), m_Coordinator->GetRemovalBoundary()};
	}

	std::string NetMatchService::ModerationStateOf(const NetSeatRoster& roster) {
		using json = nlohmann::json;
		const auto digest = [](uint64_t id) { return IdentityDigest(id); };
		json held = json::array(), tickets = json::array(), bans = json::array();
		for (const NetRosterSeat& seat: roster.seats) {
			if (seat.owner == 0) continue;
			if (seat.phase == NetSeatPhase::Held || seat.phase == NetSeatPhase::RoundEnd || seat.phase == NetSeatPhase::Relaunching)
				held.push_back({{"seat", seat.seatId}, {"cause", NetSeatHoldCauseName(seat.holdCause)}});
			tickets.push_back({{"seat", seat.seatId}, {"incarnation", seat.incarnation}, {"identity_sha256", digest(seat.owner)}});
		}
		std::vector<std::string> banned;
		for (const uint64_t id: roster.banned) banned.push_back(digest(id));
		std::sort(banned.begin(), banned.end());
		for (const std::string& one: banned) bans.push_back(one);
		return json{{"held_seats", held}, {"bans", bans}, {"tickets", tickets}}.dump();
	}

	std::string NetMatchService::GetModerationSnapshotState(bool carried) const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (carried) return m_CarriedModerationState;
		if (!m_IsHost || !m_AdmissionAttached) return {};
		const NetSeatRoster& roster = m_ReconnectHost.GetRoster();
		if (const std::pair<uint32_t, size_t> key{roster.revision, roster.banned.size()}; key != m_LiveModerationKey) {
			m_LiveModerationKey = key;
			m_LiveModerationState = ModerationStateOf(roster);
		}
		return m_LiveModerationState;
	}

	void NetMatchService::NoteWorldReleaseLocked(const std::vector<NetH4SeatStatus>& statuses, uint8_t peerId, uint16_t stableSeat, uint64_t frame) {
		WorldSeatChange release;
		release.peerId = peerId;
		release.freedSeat = stableSeat;
		release.frame = frame;
		for (const NetH4SeatStatus& status: statuses) {
			if (status.stableSeat != stableSeat) continue;
			release.hostAuthorized = status.holdCause == NetSeatHoldCause::Kicked || status.holdCause == NetSeatHoldCause::Banned || status.holdCause == NetSeatHoldCause::Released;
		}
		m_LastWorldRelease = release;
	}

	void NetMatchService::NoteWorldPromotionLocked(NetPeerId connection, uint64_t activation) {
		const NetWorldJoinSession* session = m_WorldJoin.FindSession(connection);
		if (!session) return;
		WorldSeatChange promotion;
		promotion.peerId = session->assignedPeerId;
		promotion.watcherSeat = session->stableSeat;
		promotion.connection = connection;
		promotion.frame = activation;
		if (m_LastWorldRelease && m_LastWorldRelease->peerId == promotion.peerId) {
			promotion.freedSeat = m_LastWorldRelease->freedSeat;
			promotion.hostAuthorized = m_LastWorldRelease->hostAuthorized;
		}
		m_LastWorldPromotion = promotion;
	}

	std::string NetMatchService::GetWorldOwnershipFacts() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		nlohmann::json facts = nlohmann::json::object();
		if (!m_Runner) return facts.dump();
		const NetMatchConfig& config = m_Runner->GetMatchConfig();
		// The seats are the world's slot table: the authored capacity, or every peer id but the host's when none was authored.
		uint32_t seats = 0;
		for (const uint8_t capacity: config.worldTeamCapacity) seats += capacity;
		const bool authored = seats != 0;
		if (!authored && config.persistentWorld) {
			for (uint8_t peerId = 1; peerId <= config.peerCount; ++peerId) seats += peerId != config.hostPeerId;
		}
		facts["configuration"] = {{"seats", seats}, {"capacity_authored", authored}, {"world_max_spectators", config.worldMaxSpectators}, {"persistent_world", config.persistentWorld}};
		facts["is_host"] = m_IsHost;
		facts["image_received"] = m_Runner->SawWorldImageTransfer();
		// A watcher replays the committed tail outside any round, so its own id and the ticks it watched are the service's.
		facts["local_peer"] = m_LocalPeerId;
		if (!m_IsHost && m_WorldCatchUp.snapshotTick != 0) facts["replayed"] = {{"first", m_WorldCatchUp.snapshotTick + 1}, {"last", m_WorldCatchUp.appliedThrough}};
		// A seat is named by the admission seat its lockstep id has in the round's config: the number every peer reads alike.
		nlohmann::json seatOfPeer = nlohmann::json::object();
		for (const NetH4Seat& seat: NetH4BuildSeatTable(config)) {
			if (!seat.cpu && seat.lockstepPeerId != 0) seatOfPeer[std::to_string(seat.lockstepPeerId)] = seat.stableSeat;
		}
		facts["seat_of_peer"] = seatOfPeer;
		if (m_IsHost && m_LastWorldRelease) {
			facts["release"] = {{"peer", m_LastWorldRelease->peerId}, {"freed_seat", m_LastWorldRelease->freedSeat}, {"frame", m_LastWorldRelease->frame},
			                    {"host_authorized", m_LastWorldRelease->hostAuthorized}};
		}
		if (m_IsHost && m_LastWorldPromotion) {
			const WorldSeatChange& promoted = *m_LastWorldPromotion;
			nlohmann::json promotion = {{"peer", promoted.peerId}, {"seat", seatOfPeer.value(std::to_string(promoted.peerId), nlohmann::json(nullptr))},
			                            {"freed_seat", promoted.freedSeat}, {"watcher_seat", promoted.watcherSeat}, {"host_authorized", promoted.hostAuthorized}};
			// An activation announced again later is the one that committed.
			const NetWorldJoinSession* session = m_WorldJoin.FindSession(promoted.connection);
			promotion["activation_tick"] = session && session->activationTick != 0 ? session->activationTick : promoted.frame;
			NetPeerId holder = c_InvalidNetPeerId;
			uint32_t generation = 0, incarnation = 0;
			if (m_ReconnectHost.GetSeatHolder(promoted.watcherSeat, holder, generation, incarnation)) promotion["ticket_incarnation"] = incarnation;
			if (Activity* activity = g_ActivityMan.GetActivity(); activity && WorldActivityPlayerOf(config, promoted.peerId) >= 0) {
				if (const Actor* actor = activity->GetControlledActor(WorldActivityPlayerOf(config, promoted.peerId))) promotion["actor"] = actor->GetUniqueID();
			}
			facts["promotion"] = promotion;
		}
		if (!m_IsHost && m_Session) {
			if (const NetReconnectClient* client = m_Session->GetReconnectClient(); client && client->HasRecord()) {
				facts["ticket_incarnation"] = client->GetIncarnation();
				facts["ticket_seat"] = client->GetRecord().stableSeat;
			}
		}
		if (!m_IsHost && m_WorldWatcherSeatedAt != 0) facts["watcher_seated_at"] = m_WorldWatcherSeatedAt;
		return facts.dump();
	}

	void NetMatchService::PumpSeatViews() {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_Coordinator || m_State != NetMatchServiceState::Running) {
			return;
		}
		RefreshSeatViewsLocked(NetLockstepNowMs());
		CaptureA7SeatView();
	}

	void NetMatchService::PublishModerationView() {
		if (!m_Coordinator || !m_IsHost || !m_AdmissionAttached || m_State != NetMatchServiceState::Running) return;
		auto seats = m_ReconnectHost.GetModerationView();
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
			// A player coming into the seat: how much of the world's image it has, then that it replays it.
			for (const NetWorldJoinSession& session: m_WorldJoin.Sessions()) {
				if (session.assignedPeerId != seat.lockstepPeerId) continue;
				const bool receiving = session.phase == NetWorldJoinPhase::SnapshotTransfer && session.transferStarted && session.totalChunks > 0;
				if (receiving || session.phase == NetWorldJoinPhase::CatchingUp)
					seat.joinProgress = NetSeatJoinProgress(receiving ? session.transferBytes * session.ackedChunks / session.totalChunks : 0, session.transferBytes, !receiving);
			}
		}
		m_ModerationSeats = std::move(seats);
		RefreshSeatViewsLocked(AdmissionNowMs());
	}

	void NetMatchService::PublishLobbyModerationViewLocked() {
		if (!m_IsHost || !m_AdmissionAttached || m_State != NetMatchServiceState::Starting) {
			return;
		}
		uint64_t signature = m_ReconnectHost.GetModerationSignature();
		for (const NetLobbyMember& member: m_LobbySnapshot.members) {
			signature = signature * 1099511628211ULL + member.peerId;
			for (const char letter: member.displayName) {
				signature = (signature ^ static_cast<uint8_t>(letter)) * 1099511628211ULL;
			}
		}
		if (m_LobbyModerationPublished && signature == m_LobbyModerationSignature) {
			return;
		}
		auto seats = m_ReconnectHost.GetModerationView();
		std::erase_if(seats, [](const NetH4ModerationSeat& seat) { return seat.cpu || seat.lockstepPeerId == 0; });
		for (auto& seat: seats) {
			for (const NetLobbyMember& member: m_LobbySnapshot.members) {
				if (member.peerId == seat.lockstepPeerId) { seat.displayName = member.displayName; break; }
			}
			if (!seat.substituteName.empty()) seat.displayName = seat.substituteName;
			if (seat.displayName.empty()) {
				for (const auto& previous: m_ModerationSeats) {
					if (previous.stableSeat == seat.stableSeat && previous.epoch == seat.epoch) { seat.displayName = previous.displayName; break; }
				}
			}
		}
		m_ModerationSeats = std::move(seats);
		m_LobbyModerationSignature = signature;
		m_LobbyModerationPublished = true;
	}

	void NetMatchService::AttachCoordinatorSessionSink() {
		if (!m_Coordinator) {
			return;
		}
		// What the setup worker queued while it held the round reaches the session first, in order.
		if (m_Runner)
			for (NetTransportEvent& event: m_Runner->TakeSessionTraffic()) m_PendingSessionEvents.push_back(std::move(event));
		m_Coordinator->SetSessionEventSink([this](const NetTransportEvent& event) {
			if (event.type == NetTransportEventType::PacketReceived && NetLobbyProtocol::Decode(event.bytes).ok) {
				const auto message = NetLobbyProtocol::Decode(event.bytes);
				if (const auto* config = std::get_if<NetLobbyMatchConfig>(&message.message.payload)) {
					if (!m_IsHost && m_Session && event.peerId == m_Session->GetRemoteTransportPeerId() &&
					    NetMatchConfigUtil::HashConfig(config->config) == NetMatchConfigUtil::HashConfig(m_AdoptedMatchConfig)) {
						m_PendingSessionEvents.push_back(event);
						return;
					}
				}
				if (const auto* capsule = std::get_if<NetLobbyMigration>(&message.message.payload)) {
					if (!m_IsHost && capsule->kind == 2 && m_Coordinator && m_Coordinator->UsesTransportPeer(event.peerId))
						m_PendingSessionEvents.push_back(event);
					return;
				}
				// A link carrying lobby traffic - a returner's catch-up report each second - is alive to the host's session.
				if (m_IsHost && (m_PendingTrafficNotes.empty() || m_PendingTrafficNotes.back() != event.peerId) && m_PendingTrafficNotes.size() < 256)
					m_PendingTrafficNotes.push_back(event.peerId);
				QueueLobbyEvent(event);
			} else {
				m_PendingSessionEvents.push_back(event);
			}
		});
	}

	bool NetMatchService::SealMigrationCapsule(uint8_t peerId, const NetHash32& configHash, std::vector<uint8_t>& sealed) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return SealMigrationCapsuleLocked(peerId, configHash, sealed);
	}

	bool NetMatchService::SealMigrationCapsuleLocked(uint8_t peerId, const NetHash32& configHash, std::vector<uint8_t>& sealed) {
		if (!g_SettingsMan.GetSessionDirectoryUrl().empty() && !m_DirectoryRegistered)
			return false;
		NetH4TicketRecord localTicket;
		m_TicketStore.SetPath(s_TicketStorePath.empty() ? NetReconnectTicketStore::DefaultPath() : s_TicketStorePath);
		(void)m_TicketStore.Load(UnixNowMs(nullptr), localTicket);
		const auto previousTicket = localTicket;
		if (!m_ReconnectHost.EnsureLocalTicket(localTicket))
			return false;
		localTicket.recordVersion = NetReconnectTicketStore::RecordVersionFor(localTicket.persistentWorld);
		localTicket.hostAddress = NetLanDiscovery::GetPrimaryLocalAddress() + ":" + std::to_string(m_BeaconGamePort);
		localTicket.directorySessionId = m_DirectorySessionId;
		localTicket.matchConfigHash = configHash;
		if (localTicket.issuedAtUnixMs == 0)
			localTicket.issuedAtUnixMs = UnixNowMs(nullptr);
		if (localTicket != previousTicket && !m_TicketStore.Store(localTicket))
			return false;
		const auto seats = m_ReconnectHost.GetSeatTable();
		const auto seat = std::find_if(seats.begin(), seats.end(), [&](const auto& entry) { return entry.lockstepPeerId == peerId; });
		if (seat == seats.end())
			return false;
		NetDirectoryRegisterRequest row = m_DirectoryRow;
		row.matchConfigHash = NetIdentity::HashHex(configHash);
		const uint8_t authority = m_ChatSession ? static_cast<uint8_t>(m_ChatSession->GetLocalPeerId() + 1) : m_LocalPeerId;
		std::vector<uint8_t> members{authority};
		if (m_ChatSession)
			for (const auto& peer: m_ChatSession->GetReadyPeers())
				members.push_back(static_cast<uint8_t>(peer.assignedPeerId + 1));
		std::sort(members.begin(), members.end());
		// A seat can have two live links at once - its leaver's, still up at its menu, and its substitute's - but it is one member.
		members.erase(std::unique(members.begin(), members.end()), members.end());
		const auto admission = m_ReconnectHost.ExportMigrationState();
		if (admission.empty())
			return false;
		const auto plaintext = nlohmann::json::to_cbor(nlohmann::json{{"version", 1}, {"key", m_MigrationKey}, {"admission", admission}, {"directory_row", NetDirectoryCodec::EncodeRegisterRequest(row)}, {"directory_session", m_DirectorySessionId}, {"directory_token", m_DirectoryToken}, {"authority", authority}, {"members", members}, {"generation", m_MigrationGeneration}, {"autosave_match_id", m_AutosaveMatchId}, {"autosave_round", m_AutosaveIdentity.roundId}, {"autosave_interval", m_AutosaveIdentity.intervalSeconds}});
		std::vector<uint8_t> context(configHash.begin(), configHash.end());
		context.push_back(peerId);
		return m_SeatAuth.SealForSeat(seat->stableSeat, context, plaintext, sealed);
	}

	NetResyncState NetMatchService::BuildResumeState(const NetMatchConfig& config, uint64_t savedTick, uint64_t sourceRound, const std::string& matchId, const AutosaveSideState& sideState) {
		NetResyncState state;
		state.sessionId = config.sessionId;
		state.sourceRound = sourceRound;
		state.savedTick = savedTick;
		// The control handoffs, their dropped half, how far each sender's commands were applied and the
		// round's first owner transfer are carried from the checkpoint's own manifest: every peer wrote
		// the same values at that tick, so a peer that loads its own copy and one that is streamed the
		// host's copy start the round on identical lockstep state.
		state.controlOwners = sideState.controlOwners;
		state.droppedControlOwners = sideState.droppedControlOwners;
		state.appliedCommands = sideState.appliedCommands;
		state.e2eFirstTransferUid = sideState.firstTransferUid;
		// Nothing is in flight across a restart: a command sent but not yet applied when the host died
		// is lost, and no input, command or binding is pending at the tick the world stands on.
		// The seats come from the checkpoint's own manifest, so every peer resumes onto the bindings the
		// match agreed at that tick instead of one derived here and one read out of a restored world.
		for (const auto& [peer, encoded]: sideState.playerBindings) {
			uint64_t frame = 0;
			NetGamePlayerBindings bindings;
			if (!ScenarioRunner::DecodeAgreedBindings(encoded, frame, bindings)) continue;
			state.playerBindings[peer] = NetResyncPlayerBindings{std::min(frame, savedTick), bindings};
		}
		// A seat the match never heard a binding for still needs one to launch on: its roster slot.
		for (const NetMatchPlayerSlot& slot: config.players) {
			if (slot.cpu || slot.peerId == 0 || state.playerBindings.count(slot.peerId) != 0) continue;
			NetResyncPlayerBindings binding;
			binding.frame = savedTick;
			const size_t seat = static_cast<size_t>(slot.peerId - 1);
			if (seat < binding.bindings.players.size()) {
				binding.bindings.players[seat].active = true;
				binding.bindings.players[seat].human = true;
				binding.bindings.players[seat].team = static_cast<int8_t>(slot.team);
			}
			state.playerBindings[slot.peerId] = binding;
		}
		state.rewindMatchId = matchId;
		state.rewindTick = savedTick;
		return state;
	}

	NetMatchService::WorldSegmentPlayback NetMatchService::PrepareWorldSegmentPlayback(const std::filesystem::path& directory, const NetWorldSegmentHeader& header,
	                                                                                 const NetMatchConfig& config, uint64_t firstRecordedFrame) {
		WorldSegmentPlayback staged;
		const std::string named = "checkpoint " + std::to_string(header.tick) + " of world " + header.worldId;
		std::string refusal;
		const std::optional<AutosaveDescriptor> checkpoint = AutosaveStore::Find(directory, header.worldId, header.tick, &refusal);
		if (!checkpoint) {
			staged.refusal = "segment refused: " + named + " is not on this machine";
			return staged;
		}
		if (checkpoint->worldStructureHash != header.worldDigest) {
			staged.refusal = "segment refused: " + named + " digest differs";
			return staged;
		}
		AutosaveManifest manifest;
		if (!AutosaveStore::ReadManifest(directory, header.worldId, header.tick, manifest, &refusal)) {
			staged.refusal = "segment refused: " + named + " has no restart manifest: " + refusal;
			return staged;
		}
		if (firstRecordedFrame != header.tick + 1) {
			staged.refusal = "segment refused: its first frame " + std::to_string(firstRecordedFrame) +
			                 " is not the tick after " + named;
			return staged;
		}
		staged.checkpoint = *checkpoint;
		staged.manifest = std::move(manifest);
		// The very state a resumed host stands up with: the manifest's agreed owners and applied
		// sequences at that tick, under the round the checkpoint was written in.
		staged.resumeState = BuildResumeState(config, header.tick, staged.manifest.roundId, header.worldId, staged.manifest.sideState);
		staged.startFrame = header.tick + 1;
		return staged;
	}

	bool NetMatchService::DeriveRestartKey(std::array<uint8_t, 32>& key) {
		// The store's own path decides: production leaves it at this install's identity, and a caller
		// that pointed it somewhere else means it, now that a new path unloads the key it held.
		if (!m_ParticipantStore.HasKey() && !m_ParticipantStore.LoadOrCreate(nullptr)) return false;
		return m_ParticipantStore.DeriveLocalKey(c_RestartAdmissionKeyLabel, key);
	}

	void NetMatchService::PublishRestartAdmission() {
		std::string matchId, directorySession, directoryToken, row;
		std::vector<uint8_t> state;
		uint64_t generation = 0, roundId = 0;
		uint32_t interval = 0;
		uint64_t revision = 0;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (!m_IsHost || !m_AdmissionAttached || m_AutosaveMatchId.empty() || !AutosaveStore::ValidMatchId(m_AutosaveMatchId)) return;
			// A match that writes no checkpoint has nothing to resume, so it leaves no admission file; a save the host made is one.
			if (m_MatchAutosaveSeconds == 0 && m_LastMatchSaveTime.load() == 0) return;
			matchId = m_AutosaveMatchId;
			directorySession = m_DirectorySessionId;
			directoryToken = m_DirectoryToken;
			revision = m_ReconnectHost.GetStateRevision();
			// The three cheap questions first - a new checkpoint, a moved admission plane, a moved row -
			// so an unchanged pump never pays for the export at all.
			const bool stale = m_RestartAdmissionDue.load() || revision != m_PublishedAdmissionRevision ||
			                   directorySession != m_PublishedDirectorySession || directoryToken != m_PublishedDirectoryToken;
			if (!stale) return;
			state = m_ReconnectHost.ExportMigrationState();
			// A revision that moved without changing what the export renders rewrites nothing.
			if (state.empty() || state == m_LastRestartAdmissionState) {
				m_PublishedAdmissionRevision = revision;
				m_RestartAdmissionDue.store(false);
				return;
			}
			row = NetDirectoryCodec::EncodeRegisterRequest(m_DirectoryRow);
			roundId = m_AutosaveIdentity.roundId;
			interval = m_AutosaveIdentity.intervalSeconds;
			generation = m_RestartAdmissionGeneration + 1;
		}
		std::array<uint8_t, 32> key{};
		if (!DeriveRestartKey(key)) return;
		const auto plaintext = nlohmann::json::to_cbor(nlohmann::json{{"version", 1}, {"admission", state}, {"directory_row", row},
		                                                              {"directory_session", directorySession}, {"directory_token", directoryToken},
		                                                              {"autosave_match_id", matchId}, {"autosave_round", roundId}, {"autosave_interval", interval},
		                                                              {"generation", generation}});
		AutosaveAdmission admission;
		admission.schema = AutosaveStore::c_AdmissionSchema;
		admission.matchId = matchId;
		admission.generation = generation;
		// The match id is the sealing context, so a file cannot be replayed under another match.
		const std::vector<uint8_t> context(matchId.begin(), matchId.end());
		if (!NetAuthSeal(key, context, plaintext, admission.sealed)) return;
		std::string error;
		if (!AutosaveStore::PublishAdmission(AutosaveStore::Directory(), admission, &error)) {
			{
				std::ostringstream line;
				line << "[autosave] restart admission not written: " << error;
				System::PrintDiagnosticLine(line.str());
			}
			return;
		}
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_LastRestartAdmissionState = std::move(state);
		m_PublishedDirectorySession = directorySession;
		m_PublishedDirectoryToken = directoryToken;
		m_PublishedAdmissionRevision = revision;
		m_PublishedAdmissionMatchId = matchId;
		m_RestartAdmissionGeneration = generation;
		m_RestartAdmissionDue.store(false);
	}

	void NetMatchService::WriteFinalWorldCheckpoint() {
		if (m_AutosaveMatchId.empty() || !m_Coordinator || !m_Coordinator->IsRunning()) return;
		uint64_t tick = 0;
		if (!FinalCheckpointTick(m_IsHost, m_WorldJoin.IsConfigured() && !m_WorldJoin.IsPrivateMatch(), m_FinalCheckpointWritten, m_MatchAutosaveSeconds,
		                         ScenarioRunner::IsLockstepControllerSyncActive(), g_ActivityMan.ActivityRunning(),
		                         m_Coordinator->GetResumeFrame(), tick)) {
			return;
		}
		if (!SaveStampedAutosave(tick)) {
			{
				std::ostringstream line;
				line << "[net-world] final checkpoint refused at tick=" << tick;
				System::PrintDiagnosticLine(line.str());
			}
			return;
		}
		m_FinalCheckpointWritten = true;
		// The manifest is published by the writer, so the process may not leave before it lands.
		g_ActivityMan.WaitForAutosaveTasks();
		m_RestartAdmissionDue.store(true);
		PublishRestartAdmission();
		{
			std::ostringstream line;
			line << "[net-world] final checkpoint match=" << m_AutosaveMatchId << " tick=" << tick;
			System::PrintDiagnosticLine(line.str());
		}
	}

	void NetMatchService::SweepRestartAdmission() {
		std::string matchId;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_PublishedAdmissionMatchId.empty()) return;
			// Still checkpointing that very match: the file belongs to a match that can still be resumed.
			if (m_IsHost && (m_MatchAutosaveSeconds > 0 || m_LastMatchSaveTime.load() != 0) && m_AutosaveMatchId == m_PublishedAdmissionMatchId) return;
			matchId = m_PublishedAdmissionMatchId;
			// One sweep per ended round: the check reads every archive of the match, so it may not ride
			// the pump more than once.
			m_PublishedAdmissionMatchId.clear();
		}
		if (AutosaveStore::RemoveOrphanAdmission(AutosaveStore::Directory(), matchId)) {
			{
				std::ostringstream line;
				line << "[autosave] restart admission removed: no checkpoint of match=" << matchId << " is left";
				System::PrintDiagnosticLine(line.str());
			}
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_LastRestartAdmissionState.clear();
			m_PublishedDirectorySession.clear();
			m_PublishedDirectoryToken.clear();
		}
	}

	void NetMatchService::SeatResumedWorldConfig(NetMatchConfig& config, const NetWorldIdentity& identity) {
		// The resumed round belongs to THIS boot, so nothing the previous boot signed is live under it;
		// the roster, the seats and the delay policy stay the checkpoint's own.
		config.worldId = identity.worldId;
		config.worldBoot = identity.boot;
	}

	bool NetMatchService::FinalCheckpointTick(bool host, bool worldConfigured, bool alreadyWritten, uint32_t autosaveSeconds,
	                                          bool lockstepActive, bool activityRunning, uint64_t nextFrame, uint64_t& outTick) {
		outTick = 0;
		if (!host || !worldConfigured || alreadyWritten || autosaveSeconds == 0 || !lockstepActive || !activityRunning) return false;
		if (nextFrame == 0) return false;
		outTick = nextFrame - 1;
		return outTick != 0;
	}

	bool NetMatchService::ResolveWorldResume(NetMatchServiceRequest& request, std::string* error) {
		auto refuse = [&](const std::string& reason) {
			if (error) *error = reason;
			return false;
		};
		if (!request.host || !(request.persistentWorld || request.activityPreset == "Persistent World")) return true;
		NetWorldIdentity stored;
		if (!NetWorldIdentityFile::Peek(NetWorldIdentityFile::DefaultPath(), stored, error)) return false;
		if (!NetMatchConfigUtil::IsWorldId(stored.worldId)) {
			// No record yet: this is the world's first boot and there is nothing of it to resume.
			if (!request.resumeMatchId.empty()) return refuse("-net-resume-match names no world: this install has no world identity record");
			return true;
		}
		if (!request.resumeMatchId.empty() && request.resumeMatchId != stored.worldId) {
			return refuse("-net-resume-match must name this world " + stored.worldId);
		}
		if (request.worldFresh) {
			// A fresh round opens from the scene under the same UUID; the old checkpoints stay on disk
			// for retention to prune and are refused by round once this round has checkpointed.
			request.resumeMatchId.clear();
			request.resumeTick = 0;
			// The new admission must supersede the previous round's file under this same world id.
			AutosaveAdmission previous;
			if (AutosaveStore::ReadAdmission(AutosaveStore::Directory(), stored.worldId, previous)) {
				m_RestartAdmissionGeneration = previous.generation;
			}
			return true;
		}
		const std::filesystem::path directory = AutosaveStore::Directory();
		std::optional<AutosaveDescriptor> newest;
		for (AutosaveDescriptor& held: AutosaveStore::ListRestorable(directory, stored.worldId)) {
			if (!held.resumable) continue;
			newest = std::move(held);
			break;
		}
		if (!newest) {
			// A world that has never checkpointed boots from its scene, exactly as it did before.
			request.resumeMatchId.clear();
			request.resumeTick = 0;
			return true;
		}
		if (request.resumeTick != 0 && request.resumeTick != newest->savedTick) {
			std::string reason;
			const std::optional<AutosaveDescriptor> named = AutosaveStore::Find(directory, stored.worldId, request.resumeTick, &reason);
			if (!named) return refuse("checkpoint refused: " + reason);
			// A round the world has already left cannot be re-entered: its world has moved on.
			if (named->roundId != newest->roundId) {
				return refuse("checkpoint refused: that checkpoint belongs to round " + std::to_string(named->roundId) +
				              ", the world stands on round " + std::to_string(newest->roundId));
			}
		}
		request.resumeMatchId = stored.worldId;
		return true;
	}

	bool NetMatchService::PrepareResume(NetMatchServiceRequest& request, std::string* error, const std::filesystem::path& store) {
		auto refuse = [&](const std::string& reason) {
			if (error) *error = reason;
			return false;
		};
		const std::string matchId = request.resumeMatchId;
		if (!AutosaveStore::ValidMatchId(matchId)) return refuse("resume names no match");
		const std::filesystem::path directory = store.empty() ? AutosaveStore::Directory() : store;
		std::string reason;
		std::optional<AutosaveDescriptor> checkpoint;
		if (request.resumeTick != 0) {
			checkpoint = AutosaveStore::Find(directory, matchId, request.resumeTick, &reason);
		} else {
			for (AutosaveDescriptor& held: AutosaveStore::ListRestorable(directory, matchId)) {
				if (!held.resumable) continue;
				checkpoint = std::move(held);
				break;
			}
			if (!checkpoint) reason = "this match has no resumable checkpoint";
		}
		if (!checkpoint) return refuse("checkpoint refused: " + reason);
		AutosaveManifest manifest;
		if (!AutosaveStore::ReadManifest(directory, matchId, checkpoint->savedTick, manifest, &reason)) return refuse("restart manifest refused: " + reason);
		NetMatchConfig config;
		if (!DecodeConfigPayload(manifest.configPayload, config)) return refuse("the restart manifest's configuration does not decode");
		// The hash is checked by the rule that made it, so an older build's checkpoint stays loadable.
		uint16_t hashRule = 0;
		std::string storedHash;
		if (!NetMatchConfigUtil::ParseStoredConfigHash(manifest.configHash, hashRule, storedHash)) {
			return refuse("the restart manifest's configuration hash names no hash rule");
		}
		const std::optional<NetHash32> agreedHash = NetMatchConfigUtil::HashConfigUnderRule(config, hashRule);
		if (!agreedHash) {
			return refuse("the restart manifest's configuration hash follows rule " + std::to_string(hashRule) + ", newer than this build's rule " +
			              std::to_string(NetMatchConfigUtil::c_ConfigHashRule) + ": resume it on the build that saved it");
		}
		if (NetIdentity::HashHex(*agreedHash) != storedHash) {
			return refuse("the restart manifest's configuration does not match the hash the peers agreed");
		}
		if (manifest.worldBoot != (config.persistentWorld ? config.worldBoot : 0)) {
			return refuse("the restart manifest's world boot does not match its configuration");
		}
		AutosaveAdmission admission;
		if (!AutosaveStore::ReadAdmission(directory, matchId, admission, &reason)) return refuse("admission file refused: " + reason);
		std::array<uint8_t, 32> key{};
		if (!DeriveRestartKey(key)) return refuse("this install has no identity key to open its own admission file");
		std::vector<uint8_t> plaintext;
		const std::vector<uint8_t> context(matchId.begin(), matchId.end());
		if (!NetAuthOpen(key, context, admission.sealed, plaintext)) return refuse("the admission file was not sealed by this install");
		std::vector<uint8_t> admissionState;
		std::string directorySession, directoryToken;
		uint64_t roundId = checkpoint->roundId;
		uint32_t interval = checkpoint->intervalSeconds;
		uint64_t generation = 0;
		try {
			const auto body = nlohmann::json::from_cbor(plaintext);
			if (body.at("version") != 1 || body.at("autosave_match_id").get<std::string>() != matchId) {
				std::fill(plaintext.begin(), plaintext.end(), 0);
				return refuse("the admission file names another match");
			}
			admissionState = body.at("admission").get<std::vector<uint8_t>>();
			// directory_row rides the file for the persistent-world slice that reads it; this lane
			// re-advertises the live row instead, so it is not taken here.
			(void)body.at("directory_row");
			directorySession = body.at("directory_session").get<std::string>();
			directoryToken = body.at("directory_token").get<std::string>();
			roundId = body.at("autosave_round").get<uint64_t>();
			interval = body.at("autosave_interval").get<uint32_t>();
			// The SEALED generation is the authority - it is the authenticated one - and the plaintext
			// line beside it is what the publish guard compares, so the two must agree or the file was
			// not written whole by this install.
			generation = body.at("generation").get<uint64_t>();
			if (generation != admission.generation) {
				std::fill(plaintext.begin(), plaintext.end(), 0);
				return refuse("the admission file's generation does not match its sealed export");
			}
		} catch (const nlohmann::json::exception& exception) {
			std::fill(plaintext.begin(), plaintext.end(), 0);
			return refuse(std::string("the admission file does not decode: ") + exception.what());
		}
		std::fill(plaintext.begin(), plaintext.end(), 0);
		if (admissionState.empty()) return refuse("the admission file carries no admission state");
		// The resumed lobby republishes the same roster as a new revision, so every peer acks it again.
		config.configRevision += 1;
		request.resumeConfig = config;
		request.resumeTick = checkpoint->savedTick;
		request.activityPreset = config.activityPreset;
		request.activityModule = config.activityModule;
		request.sceneName = config.sceneName;
		request.sceneModule = config.sceneModule;
		request.peerCount = config.peerCount;
		request.dedicated = config.dedicated;
		// The delay policy the peers agreed to play on, not this machine's current setting.
		request.inputDelayFrames = config.inputDelayFrames;
		request.autoInputDelay = config.delayPolicy == NetMatchDelayPolicy::Auto;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			// The next publish carries on from the generation on disk; without this the restarted host
			// offers generation 1 and the monotonic guard refuses to replace its own file.
			m_RestartAdmissionGeneration = generation;
			m_PublishedAdmissionMatchId = matchId;
			m_ResumeMatchId = matchId;
			m_ResumeTick = checkpoint->savedTick;
			m_ResumeArchiveDigest = checkpoint->worldStructureHash;
			m_ResumeSideState = manifest.sideState;
			m_ResumeAdmissionState = std::move(admissionState);
			m_ResumeDirectorySession = directorySession;
			m_ResumeDirectoryToken = directoryToken;
			m_ResumeRoundId = roundId;
			m_ResumeIntervalSeconds = interval;
		}
		{
			std::ostringstream line;
			line << "[autosave] resuming match=" << matchId << " tick=" << checkpoint->savedTick
		          << " activity=" << config.activityPreset << " peers=" << static_cast<int>(config.peerCount)
		          << " directory=" << (directorySession.empty() ? "none" : directorySession);
			System::PrintDiagnosticLine(line.str());
		}
		return true;
	}

	bool NetMatchService::OpenMigrationCapsule(const NetLobbyMigration& capsule) {
		if (HoldsServiceLock()) return OpenMigrationCapsuleLocked(capsule);
		std::lock_guard<std::mutex> lock(m_Mutex);
		return OpenMigrationCapsuleLocked(capsule);
	}

	bool NetMatchService::OpenMigrationCapsuleLocked(const NetLobbyMigration& capsule) {
		if (!m_ReconnectClient.HasRecord())
			return false;
		if (m_Coordinator && (capsule.peerId != m_LocalPeerId || capsule.configHash != m_Coordinator->GetRoundConfigHash()))
			return false;
		std::vector<uint8_t> context(capsule.configHash.begin(), capsule.configHash.end());
		context.push_back(capsule.peerId);
		std::vector<uint8_t> plaintext;
		if (!m_ReconnectClient.OpenSuccessorCapsule(context, capsule.sealedState, plaintext))
			return false;
		try {
			const auto body = nlohmann::json::from_cbor(plaintext);
			if (body.at("version") != 1)
				return false;
			const auto key = body.at("key").get<NetHash32>();
			if (std::all_of(key.begin(), key.end(), [](uint8_t value) { return value == 0; }) || body.at("generation").get<uint64_t>() < m_MigrationGeneration)
				return false;
			const auto admission = body.at("admission").get<std::vector<uint8_t>>();
			const uint8_t authority = body.at("authority").get<uint8_t>();
			const auto members = body.at("members").get<std::vector<uint8_t>>();
			if (authority == 0 || authority > NetMatchConfigUtil::c_MaxPeerCount || members.size() > NetMatchConfigUtil::c_MaxPeerCount || !std::is_sorted(members.begin(), members.end()) ||
			    std::adjacent_find(members.begin(), members.end()) != members.end() || std::find(members.begin(), members.end(), capsule.peerId) == members.end() || std::find(members.begin(), members.end(), authority) == members.end() || admission.empty() || admission.size() > 32 * 1024)
				return false;
			NetDirectoryRegisterRequest row;
			std::string reason;
			if (!NetDirectoryCodec::DecodeRegisterRequest(body.at("directory_row").get<std::string>(), row, reason))
				return false;
			const auto directorySession = body.at("directory_session").get<std::string>();
			const auto directoryToken = body.at("directory_token").get<std::string>();
			if (directorySession.size() > 128 || directoryToken.size() > 128 || (!directorySession.empty() && (directoryToken.empty() || directoryToken == directorySession)))
				return false;
			if (!m_ReconnectClient.MigrateHostContext(m_ReconnectClient.GetRecord().hostAddress, directorySession, capsule.configHash))
				return false;
			m_MigrationKey = key;
			m_MigrationAdmissionState = admission;
			{
				// What this capsule carries for moderation, read once: the roster whole and the host's ban list beside it.
				const auto carried = nlohmann::json::from_cbor(admission, true, false);
				NetSeatRoster roster;
				std::string rosterError;
				if (!carried.is_discarded() && carried.contains("roster_bytes") && DecodeRoster(carried.at("roster_bytes").get<std::vector<uint8_t>>(), roster, &rosterError)) {
					roster.banned = carried.value("roster_banned", std::vector<uint64_t>{});
					m_CarriedModerationState = ModerationStateOf(roster);
				}
			}
			m_MigrationAuthority = authority;
			m_MigrationMembers = members;
			m_MigrationGeneration = body.at("generation").get<uint64_t>();
			const auto autosaveId = body.at("autosave_match_id").get<std::string>();
			if (!autosaveId.empty() && m_AutosaveMatchId.empty()) {
				m_AutosaveMatchId = autosaveId;
				m_AutosaveIdentity.sessionId = m_ReconnectClient.GetRecord().hostSessionId;
				m_AutosaveIdentity.roundId = body.at("autosave_round").get<uint64_t>();
				m_AutosaveIdentity.intervalSeconds = body.at("autosave_interval").get<uint32_t>();
				m_AutosaveIdentity.pinnedCheckpointSource = m_PinnedAutosave;
			}
			m_MigrationDirectorySession = directorySession;
			m_MigrationDirectoryToken = directoryToken;
			{
				std::lock_guard<std::mutex> iceLock(m_MigrationIceMutex);
				m_MigrationIce.session = directorySession;
				m_MigrationIce.token = directoryToken;
			}
			m_DirectoryRow = std::move(row);
			std::fill(plaintext.begin(), plaintext.end(), 0);
			return true;
		} catch (const nlohmann::json::exception&) {
			std::fill(plaintext.begin(), plaintext.end(), 0);
			return false;
		}
	}

	void NetMatchService::PublishMigrationCapsulesLocked() {
		if (!m_IsHost || !m_Coordinator || m_Coordinator->GetConfig().matchConfig.successorOrder.empty() || !m_Session)
			return;
		const auto state = m_ReconnectHost.ExportMigrationState();
		if (state == m_LastMigrationAdmissionState && m_MigrationDirectorySession == m_DirectorySessionId && m_MigrationDirectoryToken == m_DirectoryToken)
			return;
		const auto hash = m_Coordinator->GetRoundConfigHash();
		for (const auto& peer: m_Session->GetReadyPeers()) {
			NetLobbyMigration capsule;
			capsule.kind = 2;
			capsule.peerId = static_cast<uint8_t>(peer.assignedPeerId + 1);
			capsule.configHash = hash;
			std::vector<uint8_t> bytes;
			if (!SealMigrationCapsuleLocked(capsule.peerId, hash, capsule.sealedState) || !NetLobbyProtocol::Encode({capsule}, bytes) || !ActiveWireLocked()->Send(peer.transportPeerId, NetTransportLane::ControlReliable, bytes))
				return;
		}
		m_LastMigrationAdmissionState = state;
		m_MigrationDirectorySession = m_DirectorySessionId;
		m_MigrationDirectoryToken = m_DirectoryToken;
	}

	void NetMatchService::PumpHostMigration() {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_Coordinator || !m_Session || !m_Runner)
			return;
		if (!m_IsHost)
			for (auto event = m_PendingSessionEvents.begin(); event != m_PendingSessionEvents.end();) {
				const auto decoded = event->type == NetTransportEventType::PacketReceived ? NetLobbyProtocol::Decode(event->bytes) : NetLobbyDecodeResult{};
				const auto* capsule = decoded.ok ? std::get_if<NetLobbyMigration>(&decoded.message.payload) : nullptr;
				if (capsule) {
					(void)OpenMigrationCapsuleLocked(*capsule);
					event = m_PendingSessionEvents.erase(event);
				} else
					++event;
			}
		if (m_Coordinator->IsMigrating() && m_Coordinator->GetMigrationPhase() != NetHostMigrationPhase::ResyncAdmission) {
			m_StatusText = "Host lost - arranging handover";
			return;
		}
		if (!m_Coordinator->TakeMigrationNotice()) {
			if (m_Coordinator->GetMigrationPhase() == NetHostMigrationPhase::ResyncAdmission) {
				m_Session->Tick(AdmissionNowMs());
				if (m_Session->IsReady())
					m_Coordinator->FinishMigrationAdmission();
				else if (m_Session->IsFailed() || m_Session->IsRejected())
					m_Coordinator->Complete("handover snapshot admission failed");
			}
			if (m_HeldHostStatusAtMs != 0 && SteadyNowMs() >= m_HeldHostStatusAtMs) {
				m_HeldHostStatusAtMs = 0;
				m_StatusText = "Host left - " + m_Coordinator->DescribePeer(m_LocalPeerId) + " is now hosting";
				m_MigrationStatusUntilMs = SteadyNowMs() + 3000;
				ScenarioRunner::PushNetUiToast("host_handover", m_StatusText);
			}
			if (m_MigrationStatusUntilMs != 0 && SteadyNowMs() >= m_MigrationStatusUntilMs) {
				m_StatusText = "LIVE";
				m_MigrationStatusUntilMs = 0;
				ScenarioRunner::PushNetUiToast("host_handover_live", m_StatusText);
			}
			return;
		}
		const auto& result = m_Coordinator->GetMigrationResult();
		// A handover is the survivors' election, and a held seat is a present player whose input the AI holds.
		if (std::none_of(result.members.begin(), result.members.end(), [&](uint8_t peer) { return peer != m_LocalPeerId; })) {
			const NetMatchConfig& played = m_Coordinator->GetConfig().matchConfig;
			bool liveMembersUnheard = false;
			for (const NetMatchPlayerSlot& slot: played.players) {
				const uint8_t peer = slot.peerId;
				if (slot.cpu || peer == 0 || peer == m_LocalPeerId || peer == played.hostPeerId || peer == result.hostPeerId) continue;
				if (!played.activePeerIds.empty() && std::find(played.activePeerIds.begin(), played.activePeerIds.end(), peer) == played.activePeerIds.end()) continue;
				if (m_Coordinator->HasHeldAISeat(peer) || m_Coordinator->IsPeerGoneAtFrame(peer, result.boundary)) continue;
				liveMembersUnheard = true;
			}
			switch (LoneElectionOutcome(m_Coordinator->MigrationHostAnnouncedLeave(), liveMembersUnheard)) {
				case LoneElection::EndMatch:
					System::PrintDiagnosticLine("[net-match] host left with no other survivor: the match is over for this seat");
					ScenarioRunner::SetControllerReplayError("PeerLeft:The host left the match");
					return;
				case LoneElection::RejoinHost:
					System::PrintDiagnosticLine("[net-match] host lost with no other survivor: rejoining the host instead of taking the match over");
					ScenarioRunner::SetControllerReplayError("PeerHeld:The host connection was lost - rejoining");
					return;
				case LoneElection::HostAlone:
					System::PrintDiagnosticLine("[net-match] host lost with no other connected seat: hosting the match so the held seats rejoin it");
					System::PrintDiagnosticLine("[net-match] Host left - " + m_Coordinator->DescribePeer(result.hostPeerId) + " is now hosting; boundary=" +
					                            std::to_string(result.boundary) + " round=" + std::to_string(m_Coordinator->GetRoundId()));
					break;
			}
		}
		const auto& config = m_Coordinator->GetConfig().matchConfig;
		auto wire = m_Coordinator->TakeMigrationTransport();
		if (!wire) {
			m_Coordinator->Complete("host handover lost its transport");
			return;
		}
		m_MigrationAuthority = result.hostPeerId;
		m_MigrationMembers = result.members;
		m_MigrationGeneration = result.generation;
		m_IsHost = result.hostPeerId == m_LocalPeerId;
		m_HandoverFrame = result.boundary + 1;
		m_PendingModeration.clear();
		m_PendingHostOptions.reset();
		m_HostOptionsRequest.Clear();
		m_LastRemovalIssue = {};
		m_LastKickBanResult = NetKickBanResult::ActionUnavailable;
		if (ActiveWireLocked())
			ActiveWireLocked()->Stop();
		m_MigratedTransport = std::move(wire);
		auto liveTransports = result.transports;
		if (m_IsHost)
			for (uint8_t peer: result.resyncPeers)
				liveTransports.erase(peer);
		if (m_IsHost) {
			m_BanStore.SetPath(NetHostBanStore::DefaultPath());
			(void)m_BanStore.Load(nullptr);
			m_ReconnectHost.SetBanStore(&m_BanStore);
			m_ReconnectHost.SetParticipantProofRequired(true);
			m_ReconnectHost.SetPersistentWorld(config.persistentWorld);
			m_ReconnectHost.SetDropOwnershipSource(&NetMatchService::CollectDropOwnership, this);
			m_ReconnectHost.SetSeatSimIdentitySource(&NetMatchService::SeatSimIdentitySource, this);
		}
		if (m_IsHost && !m_ReconnectHost.ImportMigrationState(m_MigrationAdmissionState, m_SeatAuth, config, m_LocalPeerId, liveTransports, AdmissionNowMs())) {
			m_Coordinator->Complete("host handover admission state is invalid");
			return;
		}
		m_PendingSessionEvents.clear();
		if (!m_Session->AdoptHostMigration(*m_MigratedTransport, m_LocalPeerId, result.hostPeerId, config, liveTransports, AdmissionNowMs())) {
			m_Coordinator->Complete("host handover session roster is invalid");
			return;
		}
		m_ChatSession = m_Session.get();
		if (m_IsHost) {
			m_Session->SetReconnectClient(nullptr);
			m_Session->SetReconnectHost(&m_ReconnectHost);
			m_Session->SetHostBanStore(&m_BanStore);
			m_Session->EnableParticipantProof(nullptr);
			m_ReconnectHost.SetDropOwnershipSource(&NetMatchService::CollectDropOwnership, this);
			m_ReconnectHost.SetSeatSimIdentitySource(&NetMatchService::SeatSimIdentitySource, this);
			m_ReconnectHost.SetLiveMatch(true);
			m_ReconnectHost.SetMigrationHold(result.snapshotProviderPeerId != 0, AdmissionNowMs());
			m_ModerationSeats = m_ReconnectHost.GetModerationView();
			const SimCensusScope census;
			m_ReconnectHost.RecordMigrationDepartures(result.boundary + 1);
			for (const auto& event: m_Coordinator->TakeMigrationAdmissionEvents()) {
				const bool survivor = std::any_of(liveTransports.begin(), liveTransports.end(), [&](const auto& peer) { return peer.second == event.peerId; });
				if (!survivor)
					m_Session->InjectEvent(event, AdmissionNowMs());
			}
		}
		m_Runner->AdoptHostMigration(result, m_LocalPeerId);
		if (m_IsHost) {
			// A returning seat's config, image and tail travel on the lobby, which is the new host's from here.
			NetLobbySessionConfig lobby;
			lobby.host = true;
			lobby.localPeerId = m_LocalPeerId;
			lobby.matchConfig = config;
			lobby.startFrame = result.boundary + 1;
			lobby.session = m_Session.get();
			lobby.sessionNowMs = [this] { return AdmissionNowMs(); };
			lobby.displayName = m_LocalName.empty() ? "Host" : m_LocalName;
			lobby.autoStart = false;
			std::string lobbyError;
			if (!m_Runner->GetLobbySession().Start(*m_MigratedTransport, lobby, &lobbyError))
				System::PrintDiagnosticLine("[net-match] the new host's rejoin lobby did not open: " + lobbyError);
		} else if (m_Coordinator->GetMigrationPhase() != NetHostMigrationPhase::ResyncAdmission) {
			// A held return reports and takes its tail on the lobby, which is the new host's from here.
			const auto hostLink = result.transports.find(result.hostPeerId);
			if (hostLink == result.transports.end() || !BindClientLobbyToHostLocked(*m_MigratedTransport, result.hostPeerId, hostLink->second, result.boundary + 1))
				System::PrintDiagnosticLine("[net-match] this peer's rejoin lobby did not follow the new host");
		}
		m_ResyncOnDesync = true;
		m_MigrationRepairPending = true;
		const auto endpoint = std::find_if(config.migrationPeers.begin(), config.migrationPeers.end(), [&](const auto& peer) { return peer.peerId == result.hostPeerId; });
		if (endpoint != config.migrationPeers.end()) {
			const std::string& connected = m_Coordinator->GetMigrationAddress();
			const std::string address = (connected.empty() ? endpoint->listenAddrs.front() : connected) + ":" + std::to_string(endpoint->listenPort);
			if (!m_IsHost) {
				(void)m_ReconnectClient.MigrateHostContext(address, m_MigrationDirectorySession, NetMatchConfigUtil::HashConfig(config));
				if (m_LastJoinRoute) {
					m_LastJoinRoute->address = address;
					m_LastJoinRoute->port = endpoint->listenPort;
					if (!m_LastJoinRoute->sessionId.empty()) m_LastJoinRoute->sessionId = m_MigrationDirectorySession;
				}
				if (m_Coordinator->GetMigrationPhase() == NetHostMigrationPhase::ResyncAdmission) {
					NetSessionConfig sessionConfig = m_Session->GetConfig();
					sessionConfig.port = endpoint->listenPort;
					sessionConfig.p2pJoin.connect = [this, peer = *endpoint](INetTransport& transport, std::string* connectError) {
						size_t nextAddress = 0;
						std::string connectedAddress;
						const NetLockstepCoordinator::MigrationIceDial dial = [this](INetTransport& link, const std::string& identity, std::string* dialError) { return DialMigrationIce(link, identity, dialError); };
						return NetLockstepCoordinator::ConnectMigrationEndpoint(transport, peer, nextAddress, connectedAddress, connectError, &dial);
					};
					std::string error;
					if (!m_Session->StartClient(*m_MigratedTransport, address, sessionConfig, &error))
						m_Coordinator->Complete("handover rejoin failed: " + error);
				}
			} else {
				m_DirectoryRow.listenAddrs = endpoint->listenAddrs;
				std::erase_if(m_DirectoryRow.listenAddrs, [](const std::string& address) { return NetLockstepCoordinator::IsMigrationIceEndpoint(address); });
				m_DirectoryRow.listenPort = endpoint->listenPort;
				m_DirectoryRow.joinMode = "ip";
				m_DirectoryRow.resumeSessionId = m_MigrationDirectorySession;
				m_DirectoryRow.resumeToken = m_MigrationDirectoryToken;
				m_DirectoryRow.name = m_LocalName;
				m_DirectoryRow.peerCount = result.members.size();
				m_DirectoryRetracted = false;
				m_MigrationDirectoryResumePending = !m_MigrationDirectorySession.empty();
				m_BeaconGamePort = endpoint->listenPort;
			}
		}
		m_IceRoute = "ip";
		m_StatusText = "Host left - " + m_Coordinator->DescribePeer(result.hostPeerId) + " is now hosting";
		m_MigrationStatusUntilMs = SteadyNowMs() + 3000;
		ScenarioRunner::PushNetUiToast("host_handover", m_StatusText);
		for (auto& member: m_LobbySnapshot.members)
			member.connected = member.cpu || std::find(result.members.begin(), result.members.end(), member.peerId) != result.members.end();
	}

	bool NetMatchService::IsHostMigrationRepairPending() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_MigrationRepairPending;
	}

	void NetMatchService::DrainPendingSessionEventsLocked(bool atTickBoundary) {
		if (!m_Session) {
			m_PendingTrafficNotes.clear();
			return;
		}
		if (!m_PendingTrafficNotes.empty()) {
			const uint64_t heardMs = AdmissionNowMs();
			for (const NetPeerId peer: m_PendingTrafficNotes) m_Session->NotePeerTraffic(peer, heardMs);
			m_PendingTrafficNotes.clear();
		}
		// A link the coordinator heard an authenticated lockstep packet on is heard by the host's session as well.
		if (m_IsHost && m_Coordinator) {
			std::vector<NetPeerId> heard;
			{
				NetLockstepPlaneGuard plane;
				const auto& peers = m_Coordinator->GetStats().peers;
				for (const auto& [peer, transport]: m_Coordinator->RemoteTransports()) {
					const auto stats = peers.find(peer);
					if (stats == peers.end() || stats->second.lastHeardMs == 0) continue;
					uint64_t& seen = m_CoordinatorHeardMs[peer];
					if (stats->second.lastHeardMs != seen) {
						seen = stats->second.lastHeardMs;
						heard.push_back(transport);
					}
				}
			}
			const uint64_t heardMs = AdmissionNowMs();
			for (const NetPeerId transport: heard) m_Session->NotePeerTraffic(transport, heardMs);
		}
		if (m_PendingSessionEvents.empty()) {
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
			if (!m_IsHost && event.type == NetTransportEventType::PacketReceived && m_Coordinator && m_Coordinator->UsesTransportPeer(event.peerId)) {
				const auto decoded = NetLobbyProtocol::Decode(event.bytes);
				if (decoded.ok)
					if (const auto* capsule = std::get_if<NetLobbyMigration>(&decoded.message.payload)) {
						(void)OpenMigrationCapsuleLocked(*capsule);
						continue;
					}
			}
			m_Session->InjectEvent(event, nowMs);
			++m_SessionEventsDrained;
		}
	}

	void NetMatchService::DiscardUndeliveredSessionEventsLocked() {
		if (!m_PendingSessionEvents.empty()) {
			m_SessionEventsDiscarded += static_cast<uint32_t>(m_PendingSessionEvents.size());
			{
				std::ostringstream line;
				line << "[net-match] discarded " << m_PendingSessionEvents.size() << " undelivered session events";
				System::PrintDiagnosticLine(line.str());
			}
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
		for (const auto& [peer, current]: stats.peers) {
			auto& total = m_LockstepTotals.peers[peer];
			total.holds += current.holds; total.substitutions += current.substitutions; total.rejoins += current.rejoins;
			total.longestWaitMs = std::max(total.longestWaitMs, current.longestWaitMs);
		}
	}

	void NetMatchService::QueueLobbyEvent(const NetTransportEvent& event) {
		if (m_PendingLobbyOverflow) { ++m_PendingLobbyDropped; return; }
		if (m_PendingLobbyEvents.size() >= 1024 || event.bytes.size() > 1024 * 1024 - m_PendingLobbyBytes) {
			m_PendingLobbyOverflow = true;
			++m_PendingLobbyDropped;
			return;
		}
		m_PendingLobbyBytes += event.bytes.size();
		m_PendingLobbyEvents.push_back(event);
	}

	void NetMatchService::NoteDroppedLobbyEvents(size_t kept) {
		if (m_PendingLobbyDropped == 0) return;
		System::PrintDiagnosticLine("[net-match] lobby event queue full: kept " + std::to_string(kept) + ", dropped " + std::to_string(m_PendingLobbyDropped) + " since the last pump");
		m_PendingLobbyDropped = 0;
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
		if (m_IsHost && m_Coordinator) {
			const uint64_t resume = m_Coordinator->GetResumeFrame();
			AnswerEndedReturnersLocked(m_CompletedRoundFinalFrame != 0 ? m_CompletedRoundFinalFrame : (resume > 0 ? resume - 1 : 0));
		}
	}

	void NetMatchService::PumpSessionEvents() {
		// Nothing sends on the wire from off this thread while this pump may change it; the end of the pump arms it again.
		DisarmHostLiveness();
		PushPendingToasts();
		PumpHostMigration();
		if (m_Coordinator && m_Coordinator->IsMigrating())
			return;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_State == NetMatchServiceState::Completed) {
				PumpCompletedSessionLocked();
				return;
			}
		}
		PumpSeatViews();
		ReportFakeLinkEffects();
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
				if (ActiveWireLocked()) PublishRelayOfferLocked(*m_Session, *ActiveWireLocked());
				ArmHostLivenessLocked();
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
			if (!m_IsHost && event.type == NetTransportEventType::PacketReceived && m_Coordinator && m_Coordinator->UsesTransportPeer(event.peerId)) {
				const auto decoded = NetLobbyProtocol::Decode(event.bytes);
				if (decoded.ok && m_Session && event.peerId == m_Session->GetRemoteTransportPeerId()) {
					if (const auto* config = std::get_if<NetLobbyMatchConfig>(&decoded.message.payload)) {
						if (NetMatchConfigUtil::HashConfig(config->config) == NetMatchConfigUtil::HashConfig(m_AdoptedMatchConfig)) {
							SetRelayOfferLocked(config->config.relay.Usable(UnixNowMs(nullptr) / 1000) ? config->config.relay : NetRelayConfig{});
							m_AdoptedMatchConfig.relay = m_RelayOffer;
							if (m_Runner) m_Runner->SetRelayOffer(m_RelayOffer);
							continue;
						}
					}
				}
				if (decoded.ok)
					if (const auto* capsule = std::get_if<NetLobbyMigration>(&decoded.message.payload)) {
						(void)OpenMigrationCapsuleLocked(*capsule);
						continue;
					}
			}
			m_Session->InjectEvent(event, nowMs);
		}
		if (hostAdmission)
			PublishMigrationCapsulesLocked();
		if (hostAdmission) {
			// The reseat is a lockstep command, not a local mutation: every peer applies the identical
			// ownership at the identical tick. QueueLocalInput restamps the sender as this peer, which
			// on the host is exactly the id MovableMan's reseat gate requires.
			DriveAutoSubstitution(nowMs);
			for (const NetGameReseat& reseat: m_ReconnectHost.TakePendingReseats()) {
				std::erase_if(m_PendingHeldReseats, [&](const auto& pending) { return pending.newOwnerPeerId == reseat.newOwnerPeerId && pending.team == reseat.team; });
				m_PendingHeldReseats.push_back(reseat);
			}
			for (auto it = m_PendingHeldReseats.begin(); it != m_PendingHeldReseats.end();) {
				const NetGameReseat& reseat = *it;
				if (m_Coordinator && m_Coordinator->HasAgreedSeatReclaim(reseat.newOwnerPeerId)) { it = m_PendingHeldReseats.erase(it); continue; }
				if (m_Coordinator && m_Coordinator->UsesBoundedWait() && m_Coordinator->HasHeldAISeat(reseat.newOwnerPeerId)) { ++it; continue; }
				if (!PrepareHeldPeerRejoinLocked(reseat.newOwnerPeerId)) { ++it; continue; }
				{
					std::ostringstream line;
					line << "[net-reconnect] reseating team " << reseat.team << " onto peer "
				          << static_cast<int>(reseat.newOwnerPeerId) << " (" << reseat.actorUIDs.size() << " actors)";
					System::PrintDiagnosticLine(line.str());
				}
				ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{ScenarioRunner::GetLockstepHostPeerId(), reseat});
				it = m_PendingHeldReseats.erase(it);
			}
			if (m_Coordinator) {
				for (const NetHoldResolutionNotice& notice: m_ReconnectHost.TakePendingHoldResolutions()) {
					std::erase_if(m_PendingHeldResolutions, [&](const auto& pending) { return pending.lockstepPeerId == notice.lockstepPeerId; });
					m_PendingHeldResolutions.push_back(notice);
				}
				for (auto it = m_PendingHeldResolutions.begin(); it != m_PendingHeldResolutions.end();) {
					const NetHoldResolutionNotice& notice = *it;
					if (notice.resolution == NetHoldResolution::Reclaimed && m_Coordinator->HasAgreedSeatReclaim(notice.lockstepPeerId)) { it = m_PendingHeldResolutions.erase(it); continue; }
					if (m_Coordinator->UsesBoundedWait() && m_Coordinator->HasHeldAISeat(notice.lockstepPeerId)) {
						++it;
						continue;
					}
					if (notice.resolution == NetHoldResolution::Reclaimed && !PrepareHeldPeerRejoinLocked(notice.lockstepPeerId)) { ++it; continue; }
					NetLockstepHoldResolution resolution = NetLockstepHoldResolution::None;
					switch (notice.resolution) {
						case NetHoldResolution::Reclaimed: resolution = NetLockstepHoldResolution::Reclaimed; break;
						case NetHoldResolution::Substituted: resolution = NetLockstepHoldResolution::Substituted; break;
					}
					m_Coordinator->ResolveHeldSeat(notice.lockstepPeerId, resolution, nowMs);
					it = m_PendingHeldResolutions.erase(it);
				}
			}
			m_SeatStatuses = m_ReconnectHost.GetSeatStatuses();
			PublishModerationView();
			CaptureA7SeatView();
		}
		if (m_IsHost && m_Coordinator && m_Coordinator->IsRunning() && (m_Coordinator->IsPersistentWorldRound() || m_WorldJoin.IsPrivateMatch())) {
			DriveWorldJoins(nowMs);
		}
		if (m_IsHost) FeedRosterReturnsLocked();
		if (m_Runner) AdoptWorldTicketSession(m_Runner->GetLobbySession().GetMatchConfig());
		DriveWorldJoinClient(nowMs);
		// The round has already said goodbye: a rejoin that lands in the drain window is answered with it,
		// never left to measure a host that is on its way out.
		if (m_IsHost && m_GoodbyeOwedToRejoiners && m_Session && m_Coordinator) AnswerEndedReturnersLocked(m_CompletedRoundFinalFrame);
		if (events.empty()) return;
		// A transport peer that reached session-Ready but carries no lockstep remote is a
		// reconnector: the host ends the round so everyone reconvenes around its snapshot.
		// A persistent world never takes that path: a fresh join is a bootstrap, not a ResyncMatch.
		if (m_IsHost && m_ResyncOnDesync && m_Coordinator && m_Coordinator->IsRunning() && !m_Coordinator->IsPersistentWorldRound() && !m_Coordinator->UsesBoundedWait()) {
			for (const NetSessionPeerInfo& peer: m_Session->GetReadyPeers()) {
				if (!m_Coordinator->UsesTransportPeer(peer.transportPeerId)) {
					if (!PrepareHeldPeerRejoinLocked(static_cast<uint8_t>(peer.assignedPeerId + 1))) continue;
					rejoinName = peer.displayName.empty() ? "a player" : peer.displayName;
					if (ClassifyRejoin(g_ActivityMan.GetActivity()) == NetRejoinAnswer::MatchOver) {
						answerMatchOver = true;
					} else {
						{
							std::ostringstream line;
							line << "[net-match] rejoin: " << rejoinName << " reconnected - resyncing the match";
							System::PrintDiagnosticLine(line.str());
						}
						m_Coordinator->RequestResync("player rejoined");
					}
					break;
				}
			}
		}
		}
		if (answerMatchOver) {
			{
				std::ostringstream line;
				line << "[net-match] rejoin: " << rejoinName << " reconnected - match is over";
				System::PrintDiagnosticLine(line.str());
			}
			AnswerMatchOverRejoin("match over");
		}
	}

	bool NetMatchService::PrepareHeldPeerRejoinLocked(uint8_t peerId) {
		if (!m_Coordinator || !m_Coordinator->UsesBoundedWait() || !m_Coordinator->HasHeldAISeat(peerId)) return true;
		if (!m_Session || !ActiveWireLocked()) return false;
		for (const auto& peer: m_Session->GetReadyPeers()) {
			if (peer.assignedPeerId + 1 != peerId) continue;
			std::string reason;
			if (m_Coordinator->PreparePeerRejoin(peerId, ActiveWireLocked()->GetPeerPingMs(peer.transportPeerId), NetLockstepNowMs(), &reason)) {
				m_RejoinFitReasons.erase(peerId);
				return true;
			}
			// A returner held back from its seat says why, once per reason.
			if (auto& said = m_RejoinFitReasons[peerId]; said != reason) {
				said = reason;
				System::PrintDiagnosticLine("[net-match] held seat peer=" + std::to_string(peerId) + " waits to rejoin: " + reason);
			}
			if (reason.starts_with("Your connection needs")) {
				m_RejoinOutcome = "waiting_for_delay";
				if (!m_WorldJoin.IsPrivateMatch()) m_Session->DisconnectReadyPeer(peer.transportPeerId, NetRejectReason::HostNotAccepting, reason);
			}
			return false;
		}
		return false;
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

	std::string NetMatchService::GetConnectedRouteLocked(uint8_t peerId) const {
		INetTransport* wire = ActiveWireLocked();
		if (!wire) return {};
		if (m_Coordinator) for (const auto& [peer, transport]: m_Coordinator->RemoteTransports()) {
			if (peerId != 0 && peer != peerId) continue;
			const auto route = wire->GetConnectedRoute(transport); if (!route.empty()) return route;
		}
		if (m_Session) for (const auto& peer: m_Session->GetReadyPeers()) {
			if (peerId != 0 && peer.assignedPeerId + 1 != peerId) continue;
			const auto route = wire->GetConnectedRoute(peer.transportPeerId); if (!route.empty()) return route;
		}
		return {};
	}

	std::string NetMatchService::GetConnectedRoute(uint8_t peerId) const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return GetConnectedRouteLocked(peerId);
	}

	NetHostHandoverState NetMatchService::GetHostHandoverState() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_Coordinator || !m_Coordinator->IsMigrating()) return NetHostHandoverState::Live;
		return m_Coordinator->GetMigrationPhase() == NetHostMigrationPhase::Contacting ? NetHostHandoverState::HostLost : NetHostHandoverState::Migrating;
	}

	NetLobbySnapshot NetMatchService::GetLobbySnapshot() const {
		// The frame's menus call this inside the plane's window, and it reads the round without the plane's lock.
		NetLockstepPlane::Gap plane("lobby snapshot");
		std::lock_guard<std::mutex> lock(m_Mutex);
		NetLobbySnapshot snapshot = m_LobbySnapshot;
		snapshot.hostPeerId = m_Coordinator ? m_Coordinator->GetHostPeerId() : m_MigrationAuthority != 0 ? m_MigrationAuthority
		                                                                   : m_Runner                    ? m_Runner->GetMatchConfig().hostPeerId
		                                                                                                 : 1;
		snapshot.serviceState = StateName(m_State);
		snapshot.statusText = m_StatusText;
		snapshot.migrating = m_Coordinator && m_Coordinator->IsMigrating();
		snapshot.hostLost = snapshot.migrating && m_Coordinator->GetMigrationPhase() == NetHostMigrationPhase::Contacting;
		if (snapshot.migrating) {
			snapshot.serviceState = snapshot.hostLost ? "HostLost" : "Migrating";
			snapshot.statusText = snapshot.hostLost ? "Host lost - contacting the next host" : "Changing hosts - recovering the shared frame";
		}
		if (!m_ErrorText.empty()) snapshot.errorText = m_ErrorText;
		snapshot.isHost = m_IsHost;
		snapshot.localPeerId = m_LocalPeerId;
		snapshot.localTeam = m_LocalTeam;
		snapshot.active = m_State != NetMatchServiceState::Idle;
		snapshot.inLobby = m_State == NetMatchServiceState::Starting || HostOptionsNeedCorrectionLocked();
		if (HostOptionsNeedCorrectionLocked()) snapshot.remoteReady = false;
		snapshot.running = m_State == NetMatchServiceState::Running || m_State == NetMatchServiceState::ReadyToLaunch;
		snapshot.failed = m_State == NetMatchServiceState::Failed;
		snapshot.playedAMatch = m_MatchWasRunning;
		snapshot.leftMatch = m_LeftMatch;
		snapshot.inputDelayText = LiveInputDelayTextLocked();
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
		if (snapshot.modeName.empty() && m_MatchConfig.sessionId != 0) {
			snapshot.modeName = NetMatchConfigUtil::ModeName(m_MatchConfig.mode);
		}
		if (snapshot.modeLabel.empty() && m_MatchConfig.sessionId != 0) {
			snapshot.modeLabel = NetMatchConfigUtil::ModeLabel(m_MatchConfig.mode);
		}
		if (snapshot.members.empty() && snapshot.active) {
			// Until the runner's first publish this renders the committed roster on the host and, on a
			// client, the local placeholder config the runner publishes from WaitForSessionReady.
			for (const NetMatchPlayerSlot& slot : m_MatchConfig.players) {
				NetLobbyMember member;
				member.peerId = slot.peerId;
				member.team = slot.team;
				member.cpu = slot.cpu;
				member.isLocal = slot.peerId == m_LocalPeerId;
				member.displayName = member.isLocal && !m_LocalName.empty() ? m_LocalName : slot.displayName;
				member.connected = member.isLocal || slot.cpu;
				member.inputDelayFrames = NetMatchConfigUtil::PeerInputDelay(m_MatchConfig, slot.peerId);
				snapshot.members.push_back(member);
			}
		}
		// A seat that landed when its host dropped names the members as of the failure: itself.
		if (m_LandedWithoutFrame) std::erase_if(snapshot.members, [](const NetLobbyMember& member) { return !member.isLocal; });
		if (snapshot.members.empty() && snapshot.active) {
			NetLobbyMember local;
			local.peerId = m_LocalPeerId;
			local.displayName = m_LocalName;
			local.team = m_LocalTeam >= 0 ? static_cast<uint8_t>(m_LocalTeam) : 0;
			local.isLocal = true;
			local.connected = true;
			snapshot.members.push_back(local);
		}
		// An unseated roster slot keeps its open name: presence still remembers the player who held
		// it, and between rounds that entry is stale - a kicked seat must not read the removed name.
		const bool persistentWorld = (m_Runner ? m_Runner->GetMatchConfig() : (m_AdoptedMatchConfig.sessionId != 0 ? m_AdoptedMatchConfig : m_MatchConfig)).persistentWorld;
		if (snapshot.joiningWorld && m_State == NetMatchServiceState::Starting && !m_SeatViews.empty()) {
			// A world's joiner lists the world's seated players from the roster the host sent, not the lobby's passing member list,
			// in which a dedicated host appears as a player and the seats come and go as their states arrive.
			const NetMatchConfig& config = m_AdoptedMatchConfig.sessionId != 0 ? m_AdoptedMatchConfig : m_MatchConfig;
			const auto own = std::find_if(snapshot.members.begin(), snapshot.members.end(), [](const NetLobbyMember& member) { return member.isLocal; });
			std::vector<NetLobbyMember> seated;
			for (const auto& [peerId, view]: m_SeatViews) {
				if (view.seat.owner == 0 && peerId != m_WorldJoinerSeatPeer) continue;
				// The lobby's rows carry the lobby's ids; only this peer's own row (its link to the host) is kept from them.
				NetLobbyMember member = peerId == m_WorldJoinerSeatPeer && own != snapshot.members.end() ? *own : NetLobbyMember{};
				member.peerId = peerId;
				member.isLocal = peerId == m_WorldJoinerSeatPeer;
				member.connected = member.isLocal || view.seat.link == NetSeatLink::Connected;
				for (const NetMatchPlayerSlot& slot: config.players)
					if (slot.peerId == peerId) member.team = slot.team;
				seated.push_back(std::move(member));
			}
			if (std::none_of(seated.begin(), seated.end(), [](const NetLobbyMember& member) { return member.isLocal; }))
				for (const NetLobbyMember& member: snapshot.members)
					if (member.isLocal) seated.insert(seated.begin(), member);
			snapshot.members = std::move(seated);
		}
		for (NetLobbyMember& member: snapshot.members) {
			const auto view = m_SeatViews.find(member.peerId);
			// A world publishes every seat as open: its roster, not the slot's label, says who sits in a seat and whether they are there.
			const bool seatedInWorld = persistentWorld && view != m_SeatViews.end() && view->second.seat.owner != 0;
			const bool unseated = !seatedInWorld && !member.connected && !member.cpu && !member.isLocal &&
			                      member.displayName == NetMatchConfigUtil::UnseatedSlotName(member.peerId, persistentWorld);
			const bool known = !unseated && view != m_SeatViews.end();
			if (known && !view->second.name.empty()) member.displayName = view->second.name;
			if (seatedInWorld && !member.isLocal) member.connected = view->second.seat.link == NetSeatLink::Connected;
			if (member.isLocal && !m_LocalName.empty() && member.displayName == NetMatchConfigUtil::UnseatedSlotName(member.peerId, persistentWorld)) member.displayName = m_LocalName;
			member.dropped = known && view->second.seat.link == NetSeatLink::Dropped && view->second.state != "Left";
			member.reclaiming = known && view->second.state == "Reconnecting";
			member.joining = member.reclaiming && view->second.seat.joining;
			member.statusLine = known ? view->second.line : std::string();
			member.connectedRoute = GetConnectedRouteLocked(member.peerId);
			if (m_Coordinator && m_State == NetMatchServiceState::Running) {
				member.inputDelayFrames = NetMatchConfigUtil::PeerInputDelay(m_Coordinator->GetConfig().matchConfig, member.peerId);
				if (const auto stats = m_Coordinator->GetStats().peers.find(member.peerId); stats != m_Coordinator->GetStats().peers.end()) {
					member.pingMs = stats->second.pingMs;
					// The panel shows this seat's current standing, not the totals a rejoined seat left behind.
					member.waits = m_Coordinator->WaitsSinceReclaim(member.peerId);
					member.longestWaitMs = m_Coordinator->LongestWaitMsSinceReclaim(member.peerId);
				}
				member.aiHeld = m_Coordinator->IsSeatUnderAI(member.peerId, m_Coordinator->GetResumeFrame());
				if (member.aiHeld) member.statusLine = member.reclaiming ? "Rejoining..." : "held - AI in control";
				if (!member.aiHeld && !member.connectedRoute.empty() && g_SettingsMan.GetNetworkShowDiagnostics())
					member.statusLine += (member.statusLine.empty() ? "" : " | ") + member.connectedRoute;
			}
		}
		return snapshot;
	}

	NetMatchConfig NetMatchService::GetLobbyMatchConfig() const {
		// The frame's menus call this inside the plane's window, and it reads the round without the plane's lock.
		NetLockstepPlane::Gap plane("lobby match config");
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_Coordinator && m_State == NetMatchServiceState::Running) return m_Coordinator->GetConfig().matchConfig;
		// A lobby publish names the agreed config on every peer; before one arrives the request's
		// own build stands in, which is what a client still shows while its lobby starts.
		return m_AdoptedMatchConfig.sessionId != 0 ? m_AdoptedMatchConfig : m_MatchConfig;
	}

	bool NetMatchService::HostOptionsNeedCorrectionLocked() const {
		return m_State == NetMatchServiceState::Completed && m_Runner && m_Runner->HasRefusedHostOptions() && !m_ErrorText.empty();
	}

	bool NetMatchService::NeedsHostOptionsCorrection() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return HostOptionsNeedCorrectionLocked();
	}

	bool NetMatchService::SubmitHostOptions(uint64_t expectedRevision, const NetMatchConfig& draft, std::string* error) {
		auto refuse = [error](const std::string& reason) {
			if (error) *error = reason;
			return false;
		};
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_IsHost) {
			return refuse("only the host submits match options");
		}
		// Only an open or completed lobby can accept the host's next configuration.
		const bool rematchLobbyUp = m_State == NetMatchServiceState::Completed && !m_LeftMatch && ActiveWireLocked() && m_Session && m_Runner;
		if (m_State != NetMatchServiceState::Starting && !rematchLobbyUp) {
			return refuse("host options apply while a lobby is open");
		}
		const NetMatchConfig& adopted = m_AdoptedMatchConfig.sessionId != 0 ? m_AdoptedMatchConfig : m_MatchConfig;
		if (expectedRevision != adopted.configRevision) {
			return refuse("the draft names a stale configuration revision");
		}
		std::string validation;
		if (!NetMatchConfigUtil::ValidateLocalAlpha(draft, &validation)) {
			return refuse(validation);
		}
		if (draft.sessionId != adopted.sessionId || draft.hostPeerId != adopted.hostPeerId || draft.peerCount != adopted.peerCount) {
			return refuse("seat capacity and session identity are fixed for the open lobby");
		}
		// A live holder's seat is never reallocated by an options edit: every adopted human slot must
		// still seat the same peer, with its team free to change. CPU seats the draft adds or drops
		// bind no transport, so they are the host's to edit.
		for (const NetMatchPlayerSlot& slot : adopted.players) {
			if (slot.cpu) continue;
			const auto kept = std::find_if(draft.players.begin(), draft.players.end(), [&slot](const NetMatchPlayerSlot& seat) {
				return !seat.cpu && seat.peerId == slot.peerId;
			});
			if (kept == draft.players.end()) {
				return refuse("the draft removes a seated player");
			}
		}
		m_PendingHostOptions = draft;
		m_PendingHostOptions->configRevision = adopted.configRevision + 1;
		// Every minute through every hour, or off; a run's own cadence override keeps its seconds.
		if (m_PendingHostOptions->autosaveIntervalSeconds != 0 && !s_AutosaveSecondsOverridden) {
			m_PendingHostOptions->autosaveIntervalSeconds = std::clamp(m_PendingHostOptions->autosaveIntervalSeconds, c_MinAutosaveIntervalSeconds, c_MaxAutosaveIntervalSeconds);
		}
		// The runner owns the lobby; this posts the accepted revision to its thread, where an open
		// round republishes it to every peer at once and a closed one starts its rematch on it. The
		// draft stays staged here too: it is what the options panel re-seeds from either way.
		m_HostOptionsRequest.Post(*m_PendingHostOptions);
		if (m_ErrorText.starts_with("Host options refused: ")) m_ErrorText.clear();
		return true;
	}

	std::optional<NetMatchConfig> NetMatchService::GetPendingHostOptions() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_PendingHostOptions;
	}

	NetHostDefaultsTemplate NetHostDefaults::FromConfig(const NetMatchConfig& config) {
		NetHostDefaultsTemplate saved;
		saved.version = c_Version;
		saved.rules = static_cast<const NetMatchStandardRules&>(config);
		saved.peerCount = config.peerCount;
		saved.dedicated = config.dedicated;
		saved.delayPolicy = config.delayPolicy;
		saved.slowPlayerBoundTicks = config.slowPlayerBoundTicks;
		saved.slowPlayerPolicy = config.slowPlayerPolicy;
		saved.inputDelayFrames = config.inputDelayFrames;
		saved.autosaveEnabled = config.autosaveEnabled;
		saved.autosaveIntervalSeconds = config.autosaveIntervalSeconds;
		saved.idleWaitMinutes = config.idleWaitMinutes;
		saved.automaticRepair = config.automaticRepair;
		saved.frameRedundancyTicks = config.frameRedundancyTicks;
		for (const NetMatchPlayerSlot& slot : config.players) {
			// A seat's position, kind and intent only: never its occupant's name or peer identity.
			saved.seats.push_back(NetHostDefaultsTemplate::Seat{
			    slot.team, slot.cpu, slot.cpu ? static_cast<uint16_t>(0) : NetMatchConfigUtil::PeerInputDelay(config, slot.peerId)});
		}
		return saved;
	}

	bool NetHostDefaults::ApplyTo(const NetHostDefaultsTemplate& saved, NetMatchConfig& config, std::string* error) {
		NetMatchConfig seeded = config;
		static_cast<NetMatchStandardRules&>(seeded) = saved.rules;
		seeded.modePreset = NetMatchConfigUtil::ModeName(saved.rules.mode);
		seeded.delayPolicy = saved.delayPolicy;
		seeded.slowPlayerBoundTicks = saved.slowPlayerBoundTicks;
		seeded.slowPlayerPolicy = saved.slowPlayerPolicy;
		seeded.inputDelayFrames = saved.inputDelayFrames;
		seeded.autosaveEnabled = saved.autosaveEnabled;
		seeded.autosaveIntervalSeconds = saved.autosaveIntervalSeconds;
		seeded.idleWaitMinutes = saved.idleWaitMinutes;
		seeded.automaticRepair = saved.automaticRepair;
		seeded.frameRedundancyTicks = saved.frameRedundancyTicks;
		// A template never reshapes a roster: the saved capacity lands only where the seats the
		// caller already built still fit inside it, and otherwise the draft keeps its own.
		const bool capacityFits = saved.peerCount >= NetMatchConfigUtil::c_MinPeerCount && saved.peerCount <= NetMatchConfigUtil::c_MaxPeerCount &&
		                          seeded.hostPeerId <= saved.peerCount &&
		                          std::none_of(seeded.players.begin(), seeded.players.end(),
		                                       [&saved](const NetMatchPlayerSlot& slot) { return slot.peerId > saved.peerCount; });
		if (capacityFits) {
			seeded.peerCount = saved.peerCount;
		}
		// Seat intent is by position and kind: the team a host wants on seat i, never who sits there.
		for (size_t seat = 0; seat < seeded.players.size() && seat < saved.seats.size(); ++seat) {
			if (seeded.players[seat].cpu == saved.seats[seat].cpu) {
				seeded.players[seat].team = saved.seats[seat].team;
			}
		}
		if (saved.delayPolicy == NetMatchDelayPolicy::Fixed) {
			// The per-sender vector is the saved intent raised to the manual floor, which is what a
			// fixed policy means; an automatic policy carries none and measures its own.
			std::vector<uint16_t> delays(seeded.peerCount, saved.inputDelayFrames);
			for (size_t seat = 0; seat < seeded.players.size() && seat < saved.seats.size(); ++seat) {
				const NetMatchPlayerSlot& slot = seeded.players[seat];
				if (!slot.cpu && slot.peerId >= 1 && slot.peerId <= seeded.peerCount) {
					delays[slot.peerId - 1] = std::max(saved.inputDelayFrames, saved.seats[seat].delayFrames);
				}
			}
			seeded.peerInputDelayFrames = std::move(delays);
		} else {
			seeded.peerInputDelayFrames.clear();
		}
		if (!NetMatchConfigUtil::ValidateLocalAlpha(seeded, error)) {
			return false;
		}
		config = std::move(seeded);
		return true;
	}

	std::string NetHostDefaults::Serialize(const NetHostDefaultsTemplate& saved) {
		auto line = [](const std::string& key, const std::string& value) { return key + " = " + value + "\n"; };
		auto number = [&line](const std::string& key, uint32_t value) { return line(key, std::to_string(value)); };
		auto flag = [&line](const std::string& key, bool value) { return line(key, value ? "1" : "0"); };
		std::string text = "// Cortex Command host match defaults, written by Host Options - Save As Host Defaults.\n";
		text += number("Version", c_Version);
		text += line("Mode", NetMatchConfigUtil::ModeName(saved.rules.mode));
		text += number("PeerCount", saved.peerCount);
		text += flag("Dedicated", saved.dedicated);
		text += line("ActivityModule", saved.rules.activityModule);
		text += line("ActivityType", saved.rules.activityType);
		text += line("ActivityPreset", saved.rules.activityPreset);
		text += line("SceneModule", saved.rules.sceneModule);
		text += line("SceneName", saved.rules.sceneName);
		text += number("Difficulty", saved.rules.difficulty);
		text += number("StartingGold", saved.rules.startingGold);
		text += flag("FogOfWar", saved.rules.fogOfWar);
		text += flag("RequireClearPathToOrbit", saved.rules.requireClearPathToOrbit);
		text += flag("DeployUnits", saved.rules.deployUnits);
		text += flag("BrainlessHumansSpectate", saved.rules.brainlessHumansSpectate);
		for (size_t team = 0; team < saved.rules.teamRules.size(); ++team) {
			const std::string prefix = "Team" + std::to_string(team);
			text += line(prefix + "Technology", saved.rules.teamRules[team].technologyIntent);
			text += line(prefix + "TechnologyModule", saved.rules.teamRules[team].technologyModule);
			text += number(prefix + "AISkill", saved.rules.teamRules[team].aiSkill);
		}
		text += line("DelayPolicy", saved.delayPolicy == NetMatchDelayPolicy::Fixed ? "fixed" : "auto");
		text += number("SlowPlayerBoundTicks", saved.slowPlayerBoundTicks);
		text += line("SlowPlayerPolicy", saved.slowPlayerPolicy == NetSlowPlayerPolicy::Pause ? "pause" : "substitute");
		text += number("InputDelayFrames", saved.inputDelayFrames);
		text += flag("AutosaveEnabled", saved.autosaveEnabled);
		text += number("AutosaveIntervalSeconds", saved.autosaveIntervalSeconds);
		text += number("IdleWaitMinutes", saved.idleWaitMinutes);
		text += flag("AutomaticRepair", saved.automaticRepair);
		text += number("FrameRedundancyTicks", saved.frameRedundancyTicks);
		text += number("SeatCount", static_cast<uint32_t>(saved.seats.size()));
		for (size_t seat = 0; seat < saved.seats.size(); ++seat) {
			const std::string prefix = "Seat" + std::to_string(seat);
			text += number(prefix + "Team", saved.seats[seat].team);
			text += flag(prefix + "CPU", saved.seats[seat].cpu);
			text += number(prefix + "Delay", saved.seats[seat].delayFrames);
		}
		return text;
	}

	bool NetHostDefaults::Parse(const std::string& text, NetHostDefaultsTemplate& out, std::string* error) {
		auto refuse = [error](const std::string& reason) {
			if (error) *error = reason;
			return false;
		};
		auto trim = [](const std::string& value) {
			const size_t first = value.find_first_not_of(" \t\r");
			const size_t last = value.find_last_not_of(" \t\r");
			return first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
		};
		auto number = [](const std::string& value, uint32_t& parsed) {
			if (value.empty() || value.size() > 10 || value.find_first_not_of("0123456789") != std::string::npos) {
				return false;
			}
			parsed = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
			return true;
		};

		NetHostDefaultsTemplate parsed;
		parsed.seats.clear();
		std::map<size_t, NetHostDefaultsTemplate::Seat> seatsByIndex;
		size_t seatCount = 0;
		bool sawVersion = false;
		size_t lineNumber = 0;
		std::istringstream lines(text);
		std::string raw;
		while (std::getline(lines, raw)) {
			++lineNumber;
			const size_t comment = raw.find("//");
			const std::string statement = trim(comment == std::string::npos ? raw : raw.substr(0, comment));
			if (statement.empty()) {
				continue;
			}
			const size_t equals = statement.find('=');
			if (equals == std::string::npos) {
				return refuse("host defaults line " + std::to_string(lineNumber) + " is not a key = value line");
			}
			const std::string key = trim(statement.substr(0, equals));
			const std::string value = trim(statement.substr(equals + 1));
			if (key.empty()) {
				return refuse("host defaults line " + std::to_string(lineNumber) + " names no key");
			}
			uint32_t asNumber = 0;
			auto readNumber = [&](uint32_t& target, const char* what) {
				if (!number(value, asNumber)) {
					return refuse(std::string("host defaults ") + what + " is not a number");
				}
				target = asNumber;
				return true;
			};
			// The version line comes first so a template a newer build wrote is refused whole, never
			// half-read into this one's fields.
			if (!sawVersion) {
				if (key != "Version") {
					return refuse("the host defaults template must begin with its Version line");
				}
				if (!number(value, asNumber) || asNumber == 0) {
					return refuse("the host defaults template names an invalid version");
				}
				if (asNumber > c_Version) {
					return refuse("the host defaults template is version " + std::to_string(asNumber) +
					              "; this build reads version " + std::to_string(c_Version));
				}
				parsed.version = static_cast<uint16_t>(asNumber);
				if (parsed.version < 2) parsed.slowPlayerPolicy = NetSlowPlayerPolicy::Pause;
				sawVersion = true;
				continue;
			}
			if (key.starts_with("Seat") && key != "SeatCount") {
				const size_t digitsEnd = key.find_first_not_of("0123456789", 4);
				uint32_t index = 0;
				if (digitsEnd == std::string::npos || digitsEnd == 4 || !number(key.substr(4, digitsEnd - 4), index) || index >= NetMatchConfigUtil::c_MaxPlayers) {
					{
						std::ostringstream line;
						line << "[net-host-defaults] ignoring unknown key '" << key << "'";
						System::PrintDiagnosticLine(line.str());
					}
					continue;
				}
				NetHostDefaultsTemplate::Seat& seat = seatsByIndex[index];
				const std::string field = key.substr(digitsEnd);
				if (field == "Team") {
					if (!number(value, asNumber)) return refuse("host defaults seat team is not a number");
					seat.team = static_cast<uint8_t>(asNumber);
				} else if (field == "CPU") {
					if (!number(value, asNumber)) return refuse("host defaults seat kind is not a number");
					seat.cpu = asNumber != 0;
				} else if (field == "Delay") {
					if (!number(value, asNumber)) return refuse("host defaults seat delay is not a number");
					seat.delayFrames = static_cast<uint16_t>(std::min<uint32_t>(asNumber, NetMatchConfigUtil::c_MaxInputDelayFrames));
				} else {
					{
						std::ostringstream line;
						line << "[net-host-defaults] ignoring unknown key '" << key << "'";
						System::PrintDiagnosticLine(line.str());
					}
				}
				continue;
			}
			if (key.starts_with("Team") && key.size() > 5) {
				uint32_t team = 0;
				if (number(key.substr(4, 1), team) && team < parsed.rules.teamRules.size()) {
					const std::string field = key.substr(5);
					if (field == "Technology") {
						parsed.rules.teamRules[team].technologyIntent = value;
						continue;
					}
					if (field == "TechnologyModule") {
						parsed.rules.teamRules[team].technologyModule = value;
						continue;
					}
					if (field == "AISkill") {
						if (!number(value, asNumber)) return refuse("host defaults team AI skill is not a number");
						parsed.rules.teamRules[team].aiSkill = static_cast<uint8_t>(asNumber);
						continue;
					}
				}
			}
			if (key == "Mode") {
				if (!NetMatchConfigUtil::ParseMode(value, parsed.rules.mode)) return refuse("host defaults names an unknown match mode");
			} else if (key == "PeerCount") {
				if (!readNumber(asNumber, "peer count")) return false;
				parsed.peerCount = static_cast<uint8_t>(asNumber);
			} else if (key == "Dedicated") {
				if (!readNumber(asNumber, "dedicated flag")) return false;
				parsed.dedicated = asNumber != 0;
			} else if (key == "ActivityModule") {
				parsed.rules.activityModule = value;
			} else if (key == "ActivityType") {
				parsed.rules.activityType = value;
			} else if (key == "ActivityPreset") {
				parsed.rules.activityPreset = value;
			} else if (key == "SceneModule") {
				parsed.rules.sceneModule = value;
			} else if (key == "SceneName") {
				parsed.rules.sceneName = value;
			} else if (key == "Difficulty") {
				if (!readNumber(asNumber, "difficulty")) return false;
				parsed.rules.difficulty = static_cast<uint8_t>(asNumber);
			} else if (key == "StartingGold") {
				if (!readNumber(parsed.rules.startingGold, "starting gold")) return false;
			} else if (key == "FogOfWar") {
				if (!readNumber(asNumber, "fog of war")) return false;
				parsed.rules.fogOfWar = asNumber != 0;
			} else if (key == "RequireClearPathToOrbit") {
				if (!readNumber(asNumber, "clear path rule")) return false;
				parsed.rules.requireClearPathToOrbit = asNumber != 0;
			} else if (key == "DeployUnits") {
				if (!readNumber(asNumber, "deploy units rule")) return false;
				parsed.rules.deployUnits = asNumber != 0;
			} else if (key == "BrainlessHumansSpectate") {
				if (!readNumber(asNumber, "spectate rule")) return false;
				parsed.rules.brainlessHumansSpectate = asNumber != 0;
			} else if (key == "DelayPolicy") {
				if (value != "auto" && value != "fixed") return refuse("host defaults names an unknown delay policy");
				parsed.delayPolicy = value == "fixed" ? NetMatchDelayPolicy::Fixed : NetMatchDelayPolicy::Auto;
			} else if (key == "SlowPlayerBoundTicks") {
				if (!readNumber(asNumber, "slow player bound") || asNumber < 1 || asNumber > NetMatchConfigUtil::c_MaxSlowPlayerBoundTicks) return refuse("host defaults slow player bound is out of range");
				parsed.slowPlayerBoundTicks = static_cast<uint16_t>(asNumber);
			} else if (key == "SlowPlayerPolicy") {
				if (value != "substitute" && value != "pause") return refuse("host defaults names an unknown slow player policy");
				parsed.slowPlayerPolicy = value == "pause" ? NetSlowPlayerPolicy::Pause : NetSlowPlayerPolicy::Substitute;
			} else if (key == "InputDelayFrames") {
				if (!readNumber(asNumber, "input delay")) return false;
				parsed.inputDelayFrames = static_cast<uint16_t>(std::min<uint32_t>(asNumber, NetMatchConfigUtil::c_MaxInputDelayFrames));
			} else if (key == "AutosaveEnabled") {
				if (!readNumber(asNumber, "autosave switch")) return false;
				parsed.autosaveEnabled = asNumber != 0;
			} else if (key == "AutosaveIntervalSeconds") {
				if (!readNumber(parsed.autosaveIntervalSeconds, "autosave interval")) return false;
			} else if (key == "IdleWaitMinutes") {
				if (!readNumber(asNumber, "idle wait")) return false;
				parsed.idleWaitMinutes = static_cast<uint8_t>(std::min<uint32_t>(asNumber, 60));
			} else if (key == "AutomaticRepair") {
				if (!readNumber(asNumber, "automatic repair")) return false;
				parsed.automaticRepair = asNumber != 0;
			} else if (key == "FrameRedundancyTicks") {
				if (!readNumber(asNumber, "frame redundancy window")) return false;
				parsed.frameRedundancyTicks = static_cast<uint8_t>(std::clamp<uint32_t>(asNumber, 1, NetMatchConfigUtil::c_MaxFrameRedundancyTicks));
			} else if (key == "SeatCount") {
				if (!readNumber(asNumber, "seat count")) return false;
				seatCount = std::min<size_t>(asNumber, NetMatchConfigUtil::c_MaxPlayers);
			} else {
				// An unknown key is a later build's field, or a hand edit: it is named and skipped,
				// never guessed at.
				{
					std::ostringstream line;
					line << "[net-host-defaults] ignoring unknown key '" << key << "'";
					System::PrintDiagnosticLine(line.str());
				}
			}
		}
		if (!sawVersion) {
			return refuse("the host defaults template has no version line");
		}
		for (size_t seat = 0; seat < seatCount; ++seat) {
			const auto found = seatsByIndex.find(seat);
			parsed.seats.push_back(found == seatsByIndex.end() ? NetHostDefaultsTemplate::Seat{} : found->second);
		}
		out = std::move(parsed);
		return true;
	}

	bool NetHostDefaults::Load(NetHostDefaultsTemplate& out, std::string* error) {
		std::string text;
		if (!g_SettingsMan.LoadNetworkHostDefaultsText(text, error)) {
			return false;
		}
		return Parse(text, out, error);
	}

	bool NetHostDefaults::Save(const NetHostDefaultsTemplate& saved, std::string* error) {
		return g_SettingsMan.SaveNetworkHostDefaultsText(Serialize(saved), error);
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
		// The frame's menus call this inside the plane's window, and it reads the round without the plane's lock.
		NetLockstepPlane::Gap plane("input delay text");
		std::lock_guard<std::mutex> lock(m_Mutex);
		return LiveInputDelayTextLocked();
	}

	std::string NetMatchService::LiveInputDelayTextLocked() const {
		if (m_Coordinator && m_State == NetMatchServiceState::Running) {
			const auto& config = m_Coordinator->GetConfig().matchConfig;
			return "Input delay: " + std::to_string(NetMatchConfigUtil::PeerInputDelay(config, m_Coordinator->GetConfig().localPeerId)) +
			    (config.delayPolicy == NetMatchDelayPolicy::Fixed ? " (fixed)" : " (auto, re-sized live)");
		}
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
		if (const auto view = m_SeatViews.find(peerId); view != m_SeatViews.end() && !view->second.name.empty()) {
			return view->second.name;
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
			{"lockstep_codec_version", config.lockstepCodecVersion}, {"match_config_version", config.matchConfigVersion},
			{"supported_lockstep_codec_version", config.supportedLockstepCodecVersion},
			{"supported_world_lockstep_codec_version", config.supportedWorldLockstepCodecVersion},
			{"supported_match_config_version", config.supportedMatchConfigVersion},
			{"supported_world_match_config_version", config.supportedWorldMatchConfigVersion},
			{"lobby_protocol_version", config.lobbyProtocolVersion}, {"committed_record_version", config.committedRecordVersion},
			{"enabled_global_scripts", config.enabledGlobalScripts}};
		const json identity{{"schema", manifest.schema}, {"build_id", manifest.buildId}, {"game_version", manifest.gameVersion},
			{"platform", manifest.platform}, {"deterministic_config", fields},
			{"module_manifest_hash", NetIdentity::HashHex(manifest.moduleManifestHash)},
			{"deterministic_config_hash", NetIdentity::HashHex(manifest.deterministicConfigHash)},
			{"session_rules_hash", NetIdentity::HashHex(manifest.sessionRulesHash)},
			{"session_identity_hash", NetIdentity::HashHex(manifest.sessionIdentityHash)}};
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_DiagnosticIdentity = identity.dump(2, ' ', false, json::error_handler_t::replace);
		++m_DiagnosticIdentityGeneration;
		// Stamped into every checkpoint this process writes, so a restore can say what it belongs to.
		m_AutosaveIdentity.buildId = manifest.buildId;
		m_AutosaveIdentity.deterministicConfigHash = NetIdentity::HashHex(manifest.deterministicConfigHash);
		m_AutosaveIdentity.moduleManifestHash = NetIdentity::HashHex(manifest.moduleManifestHash);
		m_AutosaveIdentity.sessionIdentityHash = NetIdentity::HashHex(manifest.sessionIdentityHash);
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
		NetIdentityBuildOptions options = DiagnosticIdentityOptions();
		// The world flag rides with the inputs: the build off this thread must hash the same versions.
		const bool world = m_MatchConfig.persistentWorld;
		NetIdentity::StampOptionsForTarget(options, world);
		if (!NetIdentity::CaptureManifestInputs(inputs, error, options)) return false;
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_DiagnosticIdentityInputs = std::move(inputs);
		m_DiagnosticIdentityInputsPending = true;
		m_DiagnosticIdentityInputsWorld = world;
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
		bool capturedWorld = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (!m_DiagnosticIdentityInputsPending) {
				if (error) *error = "no captured identity inputs";
				return false;
			}
			manifest = m_DiagnosticIdentityInputs;
			capturedGeneration = m_DiagnosticIdentityInputsGeneration;
			capturedWorld = m_DiagnosticIdentityInputsWorld;
			m_DiagnosticIdentityInputsPending = false;
			m_DiagnosticIdentityInputs = NetIdentityManifest{};
		}
		NetIdentityBuildOptions options = DiagnosticIdentityOptions();
		NetIdentity::StampOptionsForTarget(options, capturedWorld);
		const NetIdentityManifest captured = manifest;
		const auto started = std::chrono::steady_clock::now();
		// The game thread can write a module tree while this walk reads it, so one failed walk is retried
		// from the captured inputs before it is reported.
		bool built = NetIdentity::CompleteManifestFromInputs(manifest, error, options);
		if (!built) {
			manifest = captured;
			built = NetIdentity::CompleteManifestFromInputs(manifest, error, options);
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

	std::string NetMatchService::MemoryCensus() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::ostringstream line;
		line << m_WorldJoin.MemoryCensus();
		size_t sideBytes = 0;
		for (const auto& [tick, state]: m_WorldImageStates) sideBytes += state.sideState.capacity() + state.heldState.capacity();
		// The image's archive is shared with the writer's last autosave: the owners count says whether it is held twice.
		line << " side_states=" << m_WorldImageStates.size() << " side_state_bytes=" << sideBytes
		     << " image_archive_bytes=" << (m_WorldJoinImageArchive ? m_WorldJoinImageArchive->size() : 0) << " image_archive_owners=" << m_WorldJoinImageArchive.use_count();
		if (const auto autosave = g_ActivityMan.LastCompletedAutosave(); autosave && autosave->archive) {
			line << " autosave_archive_bytes=" << autosave->archive->size() << " autosave_archive_shared=" << (autosave->archive == m_WorldJoinImageArchive ? 1 : 0);
		}
		if (m_Runner) line << ' ' << m_Runner->GetLobbySession().MemoryCensus();
		return line.str();
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
		for (const auto& [peerId, view]: m_SeatViews) {
			if (!view.line.empty()) rosterLines.push_back(json{{"peer_id", static_cast<int>(peerId)}, {"state", view.state}, {"line", view.line}});
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
		// The seats as this peer's roster has them: the comparer joins each peer to its seat through this section.
		if (!m_SeatViews.empty()) {
			json entries = json::array();
			uint32_t revision = 0;
			for (const auto& [peerId, view]: m_SeatViews) {
				revision = view.revision;
				entries.push_back({{"stable_seat", view.stableSeat}, {"peer_id", peerId}, {"state", view.state}, {"holder_name", view.name},
				                   {"label", RosterSeatLabel(view.seat)}, {"incarnation", view.seat.incarnation}, {"phase", NetSeatPhaseName(view.seat.phase)},
				                   {"link", view.seat.link == NetSeatLink::Connected ? "connected" : "dropped"}});
			}
			reconnect["seat_snapshot"] = {{"revision", revision}, {"seats", entries}};
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
			if (s_PortMapRequested) directoryReport["observed_ip"] = m_Directory.GetObservedIp();
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
					{"relayed", (info.flags & 16) != 0},
					{"relay_pop", info.relayPop},
				};
			}
			report["p2p"]["mux"] = {{"ip_events", m_Mux ? m_Mux->IpEvents() : 0}, {"p2p_events", m_Mux ? m_Mux->P2PEvents() : 0}};
		}
#endif
		// Every live connection of the process; one on the session's wire names the player it carries.
		{
			std::map<NetPeerId, NetSessionPeerInfo> sessionPeers;
			if (m_Session) {
				for (const NetSessionPeerInfo& peer: m_Session->GetReadyPeers()) sessionPeers[peer.transportPeerId] = peer;
			}
			INetTransport* wire = ActiveWireLocked();
			auto* mux = dynamic_cast<NetMuxTransport*>(wire);
			json connections = json::array();
			for (const GnsProcessConnection& connection: GnsTransport::GetProcessConnections()) {
				const GnsPeerConnectionInfo& info = connection.info;
				// The session addresses a mux's ICE half by its tagged id.
				NetPeerId link = c_InvalidNetPeerId;
				if (connection.transport == wire || (mux && connection.transport == mux->IpGns())) {
					link = info.peerId;
				} else if (mux && connection.transport == mux->P2PGns()) {
					link = NetMuxTransport::Tag(info.peerId);
				}
				const auto peer = link == c_InvalidNetPeerId ? sessionPeers.end() : sessionPeers.find(link);
				const bool bound = peer != sessionPeers.end();
				connections.push_back(json{
				    {"transport", connection.p2p ? "ice" : "ip"},
				    {"side", connection.host ? "host" : "client"},
				    {"transport_peer_id", info.peerId},
				    {"session_wire", link != c_InvalidNetPeerId},
				    {"session_link", link},
				    {"bound", bound},
				    {"lockstep_peer_id", bound ? peer->second.assignedPeerId + 1 : 0},
				    {"display_name", bound ? peer->second.displayName : std::string()},
				    {"found", info.found},
				    {"state", info.state},
				    {"end_reason", info.endReason},
				    {"remote_identity", info.remoteIdentity},
				    {"remote_address", info.remoteAddress},
				    {"route", info.connectedRoute},
				    {"candidate", info.selectedCandidateType},
				    {"relayed", (info.flags & 16) != 0},
				    {"relay_pop", info.relayPop},
				    {"offer", info.relayOffer},
				});
			}
			report["connections"] = std::move(connections);
		}
		report["private_rejoin"] = {{"configured", m_WorldJoin.IsPrivateMatch()}, {"checkpoint_ready", m_WorldJoin.Image().IsValid()},
		    {"checkpoint_tick", m_WorldJoin.Image().tick}, {"journal_failed", m_WorldJoin.Tail().JournalFailed()}, {"error", m_PrivateJoinError},
		    {"catching_up", m_WorldCatchUp.active}, {"applied_through", m_WorldCatchUp.appliedThrough}, {"activation_frame", m_WorldCatchUp.activationTick}};
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
				peers.push_back({{"name", peer.name}, {"team", peer.team}, {"seat", peer.seat}, {"peer_id", peer.peerId}, {"input_delay", peer.inputDelayFrames},
				    {"holds", peer.holds}, {"substitutions", peer.substitutions}, {"rejoins", peer.rejoins}, {"longest_wait_ms", peer.longestWaitMs}});
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

	GnsP2PConfig NetMatchService::BuildIceConfig(const SettingsMan& settings, const std::string& localIdentity, int localVirtualPort, const NetRelayConfig& relay) {
		GnsP2PConfig config;
		config.stunServerList = settings.GetNetworkStunServers();
		config.connectionMode = static_cast<int>(settings.GetNetworkConnectionMode());
		const uint64_t now = UnixNowMs(nullptr) / 1000;
		NetRelayConfig selected = relay;
		if (settings.HasNetworkTurnServersOverride()) {
			selected = NetRelayConfig::Fixed(settings.GetNetworkTurnServers(), settings.GetNetworkTurnUser(), settings.GetNetworkTurnPass(), "override", now + 3600);
		} else if (!settings.GetNetworkPlayerTurnServers().empty()) {
			selected = NetRelayConfig::Fixed(settings.GetNetworkPlayerTurnServers(), settings.GetNetworkPlayerTurnUser(), settings.GetNetworkPlayerTurnPass(), "personal", now + 3600);
		}
		if (config.connectionMode != 1 && selected.Usable(now)) {
			selected.UdpLists(config.turnServerList, config.turnUserList, config.turnPassList);
			config.relayOffer = RelayOfferName(selected);
		}
		config.iceEnable = (config.stunServerList.empty() ? 2 : 6) | (config.turnServerList.empty() ? 0 : 1);
		if (config.connectionMode == 2) {
			config.iceEnable = 1;
			config.stunServerList.clear();
		}
		config.localIdentity = localIdentity;
		config.localVirtualPort = localVirtualPort;
		return config;
	}

	std::unique_ptr<INetTransport> NetMatchService::MakeMigrationTransport(bool ice) {
#ifdef CCCP_WITH_GNS
		if (ice) {
			const auto snapshot = m_RelaySnapshot.load();
			auto mux = std::make_unique<NetMuxTransport>();
			mux->SetHostP2P(c_MigrationVirtualPort, BuildIceConfig(g_SettingsMan, std::string(), c_MigrationVirtualPort, snapshot ? *snapshot : NetRelayConfig{}));
			return mux;
		}
#else
		(void)ice;
#endif
		return std::make_unique<GnsTransport>();
	}

	bool NetMatchService::DialMigrationIce(INetTransport& transport, const std::string& identity, std::string* error) {
#ifdef CCCP_WITH_GNS
		auto* mux = dynamic_cast<NetMuxTransport*>(&transport);
		MigrationIce ice;
		{
			std::lock_guard<std::mutex> iceLock(m_MigrationIceMutex);
			ice = m_MigrationIce;
		}
		if (!mux || !mux->P2PGns() || ice.session.empty()) {
			if (error) *error = "the handover has no ICE route: " + std::string(!mux || !mux->P2PGns() ? "its transport has no ICE half" : "no directory session reached this peer");
			return false;
		}
		auto dispatcher = std::make_shared<GnsDirectorySignalDispatcher>();
		GnsDirectorySignalDispatcher::Config config;
		config.role = GnsDirectorySignalDispatcher::Role::Joiner;
		config.baseUrl = g_SettingsMan.GetSessionDirectoryUrl();
		config.installKey = g_SettingsMan.GetOrCreateSessionDirectoryInstallKey();
		config.certPinSha256 = g_SettingsMan.GetSessionDirectoryCertSha256();
		config.sessionId = ice.session;
		if (!dispatcher->Start(*mux->P2PGns(), config)) {
			if (error) *error = "the handover's signal channel would not open";
			return false;
		}
		dispatcher->SetPolling(true, SteadyNowMs());
		mux->SetPump([dispatcher] { dispatcher->Update(SteadyNowMs()); });
		const auto snapshot = m_RelaySnapshot.load();
		NetMuxTransport::JoinSpec spec;
		spec.peerIdentity = identity;
		spec.remoteVirtualPort = c_MigrationVirtualPort;
		spec.p2p = BuildIceConfig(g_SettingsMan, std::string(), c_MigrationVirtualPort, snapshot ? *snapshot : NetRelayConfig{});
		spec.makeSignaling = [raw = dispatcher.get()] { return raw->CreateJoinSignaling(); };
		mux->SetJoinSpec(std::move(spec));
		System::PrintDiagnosticLine("[net-migration] dialing the successor's ICE route identity=" + (identity.empty() ? std::string("(any)") : identity) + " session=" + ice.session);
		return mux->Connect(std::string(), 0, error);
#else
		(void)transport; (void)identity;
		if (error) *error = "an ICE route needs GameNetworkingSockets";
		return false;
#endif
	}

	void NetMatchService::HostMigrationIce(INetTransport& listener) {
#ifdef CCCP_WITH_GNS
		auto* mux = dynamic_cast<NetMuxTransport*>(&listener);
		MigrationIce ice;
		{
			std::lock_guard<std::mutex> iceLock(m_MigrationIceMutex);
			ice = m_MigrationIce;
		}
		if (!mux || !mux->P2PGns() || ice.session.empty() || ice.token.empty()) {
			System::PrintDiagnosticLine("[net-migration] the successor's ICE route stays closed: " + std::string(!mux || !mux->P2PGns() ? "its listener has no ICE half" : "no directory credential reached this peer"));
			return;
		}
		auto dispatcher = std::make_shared<GnsDirectorySignalDispatcher>();
		GnsDirectorySignalDispatcher::Config config;
		config.role = GnsDirectorySignalDispatcher::Role::Host;
		config.baseUrl = g_SettingsMan.GetSessionDirectoryUrl();
		config.installKey = g_SettingsMan.GetOrCreateSessionDirectoryInstallKey();
		config.certPinSha256 = g_SettingsMan.GetSessionDirectoryCertSha256();
		config.sessionId = ice.session;
		config.sessionToken = ice.token;
		if (!dispatcher->Start(*mux->P2PGns(), config)) {
			System::PrintDiagnosticLine("[net-migration] the successor's signal channel would not open");
			return;
		}
		dispatcher->SetPolling(true, SteadyNowMs());
		mux->SetPump([dispatcher] { dispatcher->Update(SteadyNowMs()); });
		System::PrintDiagnosticLine("[net-migration] the successor answers ICE dials as the directory's host end: session=" + ice.session + " identity=" + GnsTransport::ProcessIdentity());
#else
		(void)listener;
#endif
	}

	void NetMatchService::SetRelayOfferLocked(const NetRelayConfig& offer) {
		if (m_RelayOffer == offer && m_RelaySnapshot.load()) return;
		if (m_RelayOffer != offer) m_RelayOfferIssuedAt = UnixNowMs(nullptr) / 1000;
		m_RelayOffer = offer;
		m_RelaySnapshot.store(std::make_shared<const NetRelayConfig>(offer));
	}

	bool NetMatchService::ReadRelayOffer(NetRelayConfig& offer) const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		offer = m_RelayOffer.Usable(UnixNowMs(nullptr) / 1000) ? m_RelayOffer : NetRelayConfig{};
		return !m_IsHost || !m_IceEnabled || m_HostRelayMode == 0 || (m_RelayReady && !m_FreshRelayRequested.load());
	}

	std::string NetMatchService::GetNatModeText() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_IceEnabled || m_IceRoute == "ip") return "Port forwarding required";
		if (m_RelayOffer.Usable(UnixNowMs(nullptr) / 1000)) return "NAT: STUN + relay";
		return g_SettingsMan.GetNetworkStunServers().empty() ? "Port forwarding required" : "NAT: STUN";
	}

	// The host row's line for an offer that expired before a renewal landed; the next offer clears it.
	static constexpr const char* c_RelayLapsedText = "Relay login expired and has not renewed yet; new joins connect direct until it does.";

	std::string NetMatchService::GetRelayError() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_RelayError;
	}

	void NetMatchService::UpdateRelayOffer(uint64_t nowMs) {
		const uint64_t wall = UnixNowMs(nullptr) / 1000;
		bool request = false;
		NetRelayConfig fixed;
		std::string matchId;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (!m_IsHost || !m_IceEnabled || m_HostRelayMode == 0) { m_RelayReady = true; m_FreshRelayRequested = false; return; }
			if (m_RelayReplies != m_Directory.IceReplies()) {
				m_RelayReplies = m_Directory.IceReplies();
				if (m_Directory.IceServers().Usable(wall)) SetRelayOfferLocked(m_Directory.IceServers());
				else if (!m_RelayOffer.Usable(wall)) SetRelayOfferLocked({});
				m_RelayError = m_Directory.IceError();
				m_RelayReady = true;
				m_RelayPublishPending = true;
			}
			if (!m_RelayOffer.Empty() && !m_RelayOffer.Usable(wall)) {
				SetRelayOfferLocked({});
				m_RelayPublishPending = true;
				if (m_RelayError.empty()) m_RelayError = c_RelayLapsedText;
			}
			if (m_Directory.GetState() != NetDirectoryClient::State::Registered || m_Directory.IceRequestPending()) return;
			const bool fresh = m_FreshRelayRequested.load();
			// Renew at half the offer's lifetime, so a connection opened on it still has the other half.
			const uint64_t issued = std::min(m_RelayOfferIssuedAt, m_RelayOffer.expiresAt);
			request = (fresh || wall >= m_RelayOffer.expiresAt - (m_RelayOffer.expiresAt - issued) / 2) && nowMs >= m_NextRelayRequestMs;
			if (!request) return;
			m_FreshRelayRequested = false;
			if (fresh) m_RelayReady = false;
			m_NextRelayRequestMs = nowMs + 15000;
			matchId = m_Directory.GetSessionId() + ":" + std::to_string(m_LastRoundId);
			if (m_HostRelayMode == 2) {
				fixed = m_FixedRelayOffer;
				if (fixed.Empty()) { m_RelayReady = true; return; }
				fixed.matchId = matchId;
				fixed.expiresAt = wall + NetDirectoryClient::c_MaxRelayTtlSeconds;
				SetRelayOfferLocked(fixed);
				m_RelayPublishPending = true;
				if (m_RelayError == c_RelayLapsedText) m_RelayError.clear();
			}
		}
		if (request) m_Directory.RequestIceServers(matchId, NetDirectoryClient::c_MaxRelayTtlSeconds, fixed.Empty() ? nullptr : &fixed);
	}

	void NetMatchService::PublishRelayOfferLocked(NetSession& session, INetTransport& wire) {
		if (!m_IsHost || !m_RelayPublishPending || !m_Runner) return;
		NetMatchConfig config = m_Runner->GetMatchConfig();
		config.relay = m_RelayOffer.Usable(UnixNowMs(nullptr) / 1000) ? m_RelayOffer : NetRelayConfig{};
		std::vector<uint8_t> bytes;
		if (!NetLobbyProtocol::Encode({NetLobbyMatchConfig{config}}, bytes)) return;
		bool sent = true;
		for (const auto& peer : session.GetReadyPeers()) {
			sent = wire.Send(peer.transportPeerId, NetTransportLane::ControlReliable, bytes) && sent;
		}
		m_Runner->SetRelayOffer(config.relay);
		m_AdoptedMatchConfig.relay = config.relay;
		m_MatchConfig.relay = config.relay;
		m_RelayPublishPending = !sent;
	}

	bool NetMatchService::SetUpIceTransport(const NetMatchServiceRequest& request, const NetIdentityManifest& manifest, NetMuxTransport& mux, NetSessionConfig& sessionConfig, std::string& joinAddress, NetIceJoinTarget& target, std::string* error) {
#ifndef CCCP_WITH_GNS
		(void)request; (void)manifest; (void)mux; (void)sessionConfig; (void)joinAddress; (void)target;
		if (error) *error = "a session-id join needs GameNetworkingSockets";
		return false;
#else
		const std::string baseUrl = g_SettingsMan.GetSessionDirectoryUrl();
		const std::string installKey = g_SettingsMan.GetOrCreateSessionDirectoryInstallKey();
		const std::string certPin = g_SettingsMan.GetSessionDirectoryCertSha256();
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
			NetRelayConfig relay;
			const uint64_t relayDeadline = SteadyNowMs() + c_IceConnectBudgetMs;
			while (!ReadRelayOffer(relay) && !m_CancelRequested.load() && SteadyNowMs() < relayDeadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
			const GnsP2PConfig ice = BuildIceConfig(g_SettingsMan, identity, c_IceVirtualPort, relay);
			if (ice.connectionMode == 2 && ice.turnServerList.empty()) {
				if (error) *error = "Relay setup failed: no unexpired relay credentials; configure a relay or choose Automatic";
				return false;
			}
			mux.SetHostP2P(c_IceVirtualPort, ice);
			m_Dispatcher->SetPolling(true, SteadyNowMs());
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_RelayAttempted = !ice.turnServerList.empty();
				m_IceBoundSessionId = sessionId;
				m_IceIdentity = identity;
				m_IceRoute = "ice";
			}
			{
				std::ostringstream line;
				line << "[net-ice] host session " << sessionId << " identity " << identity << " listening on virtual port " << c_IceVirtualPort;
				System::PrintDiagnosticLine(line.str());
			}
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

		bool reservedSeat = false;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			NetH4TicketRecord record;
			m_TicketStore.SetPath(s_TicketStorePath.empty() ? NetReconnectTicketStore::DefaultPath() : s_TicketStorePath);
			reservedSeat = m_TicketStore.Load(UnixNowMs(nullptr), record, nullptr) == NetH4TicketLoadResult::Loaded &&
			    TicketMatchesRequest(record, request.sessionId, request.address);
		}
		std::string why = "no such session";
		const uint64_t deadline = SteadyNowMs() + c_IceResolveBudgetMs;
		while (SteadyNowMs() < deadline && !m_CancelRequested.load()) {
			const uint64_t nowMs = SteadyNowMs();
			browse.PollList(nowMs);
			browse.Update(nowMs);
			if (browse.ListReplies() > 0) {
				why = NetIceResolveSessionRow(browse.Rows(), local, request.sessionId, &target, worldIdentityBuilt ? &worldLocal : nullptr, reservedSeat);
				if (why.empty() || why != "no such session") {
					break;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		browse.StopBrowsing();
		if (!why.empty()) {
			if (why == "modules") {
				NetReportGameData(manifest);
				for (const NetDirectorySessionRow& row : browse.Rows()) {
					if (row.sessionId != request.sessionId) continue;
					NetDirectoryClient::GameRow refused;
					refused.reason = why;
					refused.localModuleManifestHash = (row.persistentWorld && worldIdentityBuilt ? worldLocal : local).moduleManifestHash;
					refused.hostModuleManifestHash = row.moduleManifestHash;
					// The player reads the sentence; the session id is the log's.
					System::PrintDiagnosticLine("[net-ice] session " + request.sessionId + " refused: modules");
					if (error) *error = NetDirectoryClient::JoinRefusalText(refused);
					return false;
				}
			}
			if (error) *error = "session " + request.sessionId + ": " + why;
			return false;
		}
		if (target.persistentWorld && worldIdentityBuilt) {
			sessionConfig.localIdentity = worldManifest;
		}

		// Automatic prefers ICE because a directory address can be private.
		if (!NetIcePrefersP2P(target, m_IceEnabled)) {
			if (g_SettingsMan.GetNetworkConnectionMode() == SettingsMan::NetworkConnectionMode::RelayOnly) {
				if (error) *error = "Relay only requires a host that offers an Internet ICE join";
				return false;
			}
			if (target.address.empty() || target.port == 0) {
				if (error) *error = "NAT traversal is Off: enable Automatic or ask the host for a forwarded UDP address";
				return false;
			}
			joinAddress = target.address;
			sessionConfig.port = target.port;
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_IceRoute = "ip";
			}
			{
				std::ostringstream line;
				line << "[net-ice] session " << request.sessionId << " join_mode=" << target.joinMode << " resolved to " << target.address << ":" << target.port << "; taking the IP half";
				System::PrintDiagnosticLine(line.str());
			}
			return true;
		}

		config.role = GnsDirectorySignalDispatcher::Role::Joiner;
		config.sessionId = request.sessionId;
		if (!m_Dispatcher->Start(*mux.P2PGns(), config)) {
			if (error) *error = "ICE signaling failed: the joiner signal channel would not open";
			return false;
		}
		m_Dispatcher->SetPolling(true, SteadyNowMs());
		NetMuxTransport::JoinSpec spec;
		spec.peerIdentity = target.identity;
		spec.remoteVirtualPort = c_IceVirtualPort;
		NetRelayConfig relay;
		if (g_SettingsMan.GetNetworkConnectionMode() != SettingsMan::NetworkConnectionMode::DirectOnly && g_SettingsMan.GetNetworkPlayerTurnServers().empty() && !g_SettingsMan.HasNetworkTurnServersOverride()) {
			browse.FetchIceServers(request.sessionId);
			const uint64_t relayDeadline = SteadyNowMs() + c_IceConnectBudgetMs;
			while (browse.IceRequestPending() && !m_CancelRequested.load() && SteadyNowMs() < relayDeadline) {
				browse.Update(SteadyNowMs());
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
			relay = browse.IceServers();
		}
		spec.p2p = BuildIceConfig(g_SettingsMan, std::string(), c_IceVirtualPort, relay);
		if (spec.p2p.connectionMode == 2 && spec.p2p.turnServerList.empty()) {
			// Relay only never falls back: the player is told why there is no relay and what to change.
			if (error) {
				*error = !g_SettingsMan.GetNetworkPlayerTurnServers().empty() || g_SettingsMan.HasNetworkTurnServersOverride()
				             ? "Your relay has no unexpired login - check your relay setting or switch Connection to Automatic"
				         : browse.IceRelayRefused() ? "The host's relay refused the credentials - ask the host to check the relay setting"
				                                    : "No relay is configured for this match - switch Connection to Automatic or Direct only";
			}
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			SetRelayOfferLocked(relay);
			m_RelayAttempted = !spec.p2p.turnServerList.empty();
		}
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
		{
			std::ostringstream line;
			line << "[net-ice] session " << request.sessionId << " join_mode=" << target.joinMode << " resolved to identity " << target.identity << "; dialling the ICE half";
			System::PrintDiagnosticLine(line.str());
		}
		return true;
#endif
	}

	bool NetMatchService::StartLobbyConnection(std::unique_ptr<NetMuxTransport>& mux, INetTransport& ip, NetSession& session, NetLockstepCoordinator& coordinator,
	                                         NetMatchRunner& runner, NetMatchRunnerConfig& config, const NetIceJoinTarget& target,
	                                         bool transportReady, bool& noDirectRoute, std::string* error) {
		noDirectRoute = false;
		const uint32_t directTimeoutMs = config.sessionConfig.timeoutMs;
		const bool iceDial = !config.host && config.sessionConfig.p2pJoin.connect;
		if (iceDial) {
			m_IceDialRetrying = false;
			// The host's JoinAccepted restores its heartbeat timeout after candidate gathering; the session outlives the transport's connect limit.
			config.sessionConfig.timeoutMs = std::max(directTimeoutMs, GnsTransport::IceConnectTimeoutMs() + c_IceHandshakeMarginMs);
			ArmIceConnectingLine(config, session);
		}
		INetTransport& wire = mux ? static_cast<INetTransport&>(*mux) : ip;
		// Directory lookup time is not part of either transport's connection deadline.
		if (config.nowMs) session.Tick(config.nowMs(), false);
		if (transportReady && runner.Start(wire, session, coordinator, config, error)) return true;
		if (config.host || !NetIcePrefersP2P(target, m_IceEnabled) || m_CancelRequested.load()) return false;
		const auto routeFailed = [&] {
			const bool unconnectedClose = session.IsClosed() && (session.GetRejectReason() == NetRejectReason::InternalError || session.GetRejectReason() == NetRejectReason::HostLinkLost) && session.GetMismatchKey().empty();
			return runner.GetLobbySession().GetState() == NetLobbyState::Idle &&
			       (session.IsFailed() || session.IsClosed() || session.GetState() == NetSessionState::Connecting) &&
			       session.GetRemoteTransportPeerId() == c_InvalidNetPeerId && !session.IsRejected() &&
			       (!session.HasReject() || session.GetMismatchKey() == "transport" || session.GetMismatchKey() == "timeout_ms" || unconnectedClose);
		};
#ifdef CCCP_WITH_GNS
		// A dial that ran out of time while its host was answering gets one more: the slow part was the exchange, not the host.
		if (iceDial && transportReady && routeFailed() && m_Dispatcher) {
			const GnsDirectorySignalDispatcher::Counters counters = m_Dispatcher->GetCounters();
			const bool retry = NetIceRetryCanSucceed(counters.signalsIn, counters.refusals);
			System::PrintDiagnosticLine("[net-ice] connect failed after " + std::to_string((SteadyNowMs() - m_IceDialStartedMs) / 1000) + " s (signals from the host " +
			                            std::to_string(counters.signalsIn) + ", refusals " + std::to_string(counters.refusals) + "): " +
			                            (retry ? "dialling once more" : "no retry, the host never answered") + "; " + (error ? *error : std::string()));
			if (retry) {
				session.Close("retry ICE");
				if (error) error->clear();
				m_IceDialRetrying = true;
				ArmIceConnectingLine(config, session);
				if (config.nowMs) session.Tick(config.nowMs(), false);
				if (runner.Start(wire, session, coordinator, config, error)) return true;
				if (m_CancelRequested.load()) return false;
			}
		}
#endif
		if (m_ConnectionMode == 2) {
			// A setup that never reached the transport already says why; only a relay that failed to connect is named here.
			if (error && transportReady) *error = "Relay connection failed: " + *error + "; check the relay or choose Automatic";
			return false;
		}
		if (transportReady && !routeFailed()) return false;
		noDirectRoute = true;
		const std::string iceError = (transportReady ? (m_RelayAttempted ? "ICE direct/relay connection failed: " : "ICE connection failed: ") : "") + (error ? *error : std::string());
		if (target.address.empty() || target.port == 0) {
			if (error) *error = iceError + "; no direct IP address advertised";
			return false;
		}

		session.Close("retry direct IP");
		if (mux) mux->SetPump({});
#ifdef CCCP_WITH_GNS
		if (m_Dispatcher) {
			m_Dispatcher->Stop();
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_IceReport = m_Dispatcher->BuildReportJson();
			m_Dispatcher.reset();
		}
#endif
		if (mux) mux->Stop();
		config.sessionConfig.p2pJoin = {};
		config.sessionConfig.timeoutMs = directTimeoutMs;
		config.sessionConfig.port = target.port;
		config.joinAddress = target.address;
		// SessionFull retries must keep this leg instead of resolving back to ICE.
		config.resolveJoinAddress = {};
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_IceRoute = "ip";
			m_StatusText = "NAT traversal failed; trying the host's UDP address";
		}
		System::PrintDiagnosticLine("[net-ice] " + iceError + "; retrying IP " + target.address + ":" + std::to_string(target.port));
		if (error) error->clear();
		if (config.nowMs) session.Tick(config.nowMs(), false);
		const bool started = runner.Start(ip, session, coordinator, config, error);
		// The session has released the old wire before its owner is destroyed.
		mux.reset();
		noDirectRoute = !started && !m_CancelRequested.load() && routeFailed();
		if (noDirectRoute && error) *error = iceError + "; IP connection failed: " + *error;
		return started;
	}

	void NetMatchService::ArmIceConnectingLine(NetMatchRunnerConfig& config, NetSession& session) {
		m_IceDialStartedMs = SteadyNowMs();
		m_IceConnectingPhase.clear();
		m_IceSignalsAtDial = 0;
#ifdef CCCP_WITH_GNS
		if (m_Dispatcher) m_IceSignalsAtDial = m_Dispatcher->GetCounters().signalsIn;
#endif
		if (config.publishLobby && !m_IceDialRetrying) {
			// Runs on the worker thread beside the dial; it stops naming the dial the moment the transport connects.
			config.publishLobby = [this, &session, publish = config.publishLobby](const NetLobbySnapshot& snapshot) {
				if (session.GetRemoteTransportPeerId() == c_InvalidNetPeerId && session.GetState() == NetSessionState::Connecting) {
					UpdateIceConnectingLine();
				}
				publish(snapshot);
			};
		}
	}

	void NetMatchService::UpdateIceConnectingLine() {
		bool answered = false;
#ifdef CCCP_WITH_GNS
		answered = m_Dispatcher && m_Dispatcher->GetCounters().signalsIn > m_IceSignalsAtDial;
#endif
		const std::string phase = m_IceDialRetrying ? (answered ? "retry, testing routes" : "retry, waiting for the host") : (answered ? "testing routes" : "waiting for the host's answer");
		const std::string line = NetIceConnectingLine(SteadyNowMs() - m_IceDialStartedMs, GnsTransport::IceConnectTimeoutMs(), answered, m_RelayAttempted, m_IceDialRetrying);
		if (phase != m_IceConnectingPhase) {
			m_IceConnectingPhase = phase;
			System::PrintDiagnosticLine("[net-ice] connecting: " + line);
		}
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_StatusText = line;
	}

	std::string NetMatchService::SetupFailureStatus(const NetSession* session, bool noDirectRoute, bool relayFailed) {
		if (session && session->HasReject()) {
			if (session->GetRejectReason() == NetRejectReason::ParticipantRemoved) return "The host removed you from this session";
			if (session->GetRejectReason() == NetRejectReason::ParticipantBanned) return session->BuildRejectText();
			if (session->GetRejectSummary() == "Match roster refused") return "Match roster refused";
			if (!noDirectRoute && !relayFailed) return session->BuildPlayerRefusalText();
		}
		if (relayFailed) return "Relay route failed (TURN): check the relay or forward the host's UDP port";
		return noDirectRoute ? "No direct route (NAT): forward the host's UDP port or use LAN" : "Network setup failed";
	}

	void NetMatchService::ConfigureLobbyStart(NetMatchRunnerConfig& config) {
		config.relayOffer = [this](NetRelayConfig& offer) { return ReadRelayOffer(offer); };
		config.sessionWaitMs = c_MenuLobbyWaitMs;
		config.lobbyWaitMs = c_MenuLobbyWaitMs;
		if (config.host) {
			config.lobbySeatingWaitMs = !config.matchConfig.persistentWorld && config.matchConfig.idleWaitMinutes > 0
			                              ? static_cast<uint32_t>(config.matchConfig.idleWaitMinutes) * 60000 : 0u;
		}
		config.autoReady = config.host;
		// World hosts, including the menu path, start automatically once their bound players are ready.
		config.autoStart = config.host && config.matchConfig.persistentWorld;
		config.readyRequested = &m_ReadyRequested;
		config.startRequested = &m_StartRequested;
		config.roundStartScripts = [this] {
			std::lock_guard<std::mutex> lock(m_Mutex);
			return std::exchange(m_RoundStartScriptsToStream, {});
		};
		config.cancelRequested = &m_CancelRequested;
		config.hostOptions = &m_HostOptionsRequest;
	}

	void NetMatchService::WorkerMain(NetMatchServiceRequest request, NetIdentityManifest manifest, NetIdentityBuildOptions identityOptions) {
		std::string identityError;
		const bool identityReady = !m_CancelRequested.load() && NetIdentity::CompleteManifestFromInputs(manifest, &identityError, identityOptions);
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_IdentityPending = false;
			if (!identityReady || m_CancelRequested.load()) {
				m_State = NetMatchServiceState::Failed;
				m_StatusText = "Identity build failed";
				m_ErrorText = identityError.empty() ? "match setup canceled" : identityError;
				m_WorkerDone = true;
				return;
			}
			m_DirectoryRow.sessionIdentityHash = NetIdentity::HashHex(manifest.sessionIdentityHash);
			m_DirectoryRow.moduleManifestHash = NetIdentity::HashHex(manifest.moduleManifestHash);
		}
		CacheDiagnosticIdentity(manifest);
		if (request.host) NetReportGameData(manifest);
		if (request.host) {
			// Arm the off-sim reconnect-auth epoch; without real crypto nothing is issued (fail closed).
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_SeatAuth.BeginHostedSession()) {
				(void)GetNetAuthCrypto().RandomBytes(m_MigrationKey.data(), m_MigrationKey.size());
				System::PrintDiagnosticLine("[net-auth] reconnect-auth epoch armed");
			} else {
				System::PrintDiagnosticLine("[net-auth] crypto unavailable - reconnect auth disabled");
			}
		}
		auto transport = std::make_unique<GnsTransport>();
		auto session = std::make_unique<NetSession>();
		auto coordinator = std::make_unique<NetLockstepCoordinator>();
		auto runner = std::make_unique<NetMatchRunner>();
		bool iceWanted = false;
		const bool resolveDirectory = !request.host && !request.sessionId.empty();
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			iceWanted = m_IceEnabled;
			// The worker owns the session through the whole lobby; chat still needs to reach it.
			m_ChatSession = session.get();
		}
		std::unique_ptr<NetMuxTransport> mux;
		if (iceWanted || resolveDirectory) {
			mux = std::make_unique<NetMuxTransport>();
		}

		NetMatchRunnerConfig runnerConfig;
		runnerConfig.host = request.host;
		runnerConfig.waitForSlot = !request.host && std::exchange(s_WaitForSlotOnce, false);
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
		bool started = request.resumeConfig ? (runnerConfig.matchConfig = *request.resumeConfig, true)
		                                    : BuildMatchConfig(request, c_UiSessionId, runnerConfig.matchConfig, &error);
		runnerConfig.sessionConfig = BuildSessionConfig(manifest, request, runnerConfig.matchConfig);
		runnerConfig.autoInputDelay = request.autoInputDelay;
		runnerConfig.useLobbyProtocol = true;
		ConfigureLobbyStart(runnerConfig);
		// First lockstep tick is 1: RestartActivity zeroes the sim count, UpdateSim increments it before MovableMan reads it.
		runnerConfig.startFrame = 1;
		// A resumed match starts on the tick after the checkpoint, exactly as a healed round resumes
		// behind the snapshot it reloaded.
		if (request.resumeConfig && request.resumeTick != 0) {
			runnerConfig.startFrame = ScenarioRunner::ResyncResumeStartFrame(request.resumeTick + 1);
		}
		// Every peer answers whether it holds the checkpoint; one that does is streamed nothing.
		runnerConfig.resumeHeld = [this](const NetLobbyResume& offer) { return AnswerResumeOffer(offer); };
		NetMatchRunner* runnerRaw = runner.get();
		runnerConfig.publishLobby = [this, runnerRaw](const NetLobbySnapshot& snapshot) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_LobbySnapshot = snapshot;
			// A world's joiner that has begun to receive the image comes into a running world: nobody there readies up.
			m_LobbySnapshot.joiningWorld = !m_IsHost && runnerRaw->SawWorldImageTransfer() &&
			                               (runnerRaw->GetLobbySession().GetState() != NetLobbyState::Idle ? runnerRaw->GetLobbySession().GetMatchConfig() : runnerRaw->GetMatchConfig()).persistentWorld;
			// The announced delay comes from the lobby's exchanged config (host-authored, already
			// auto-adjusted) — never recomputed here, so every peer renders the same value.
			const NetMatchConfig& config = runnerRaw->GetState() != NetMatchRuntimeState::Running && runnerRaw->GetLobbySession().GetState() != NetLobbyState::Idle
			                                   ? runnerRaw->GetLobbySession().GetMatchConfig()
			                                   : runnerRaw->GetMatchConfig();
			// The options view reads the same agreed config on every peer; the mirror sits under the
			// same lock the snapshot publish already holds.
			m_AdoptedMatchConfig = config;
			if (!m_IsHost) SetRelayOfferLocked(config.relay.Usable(UnixNowMs(nullptr) / 1000) ? config.relay : NetRelayConfig{});
			AdoptWorldTicketSession(config);
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
			// The host's seat panel moderates from the admission rows, and the lobby is where it most
			// needs them; they are named from the roster that has just been published.
			PublishLobbyModerationViewLocked();
			// A world's joiner sits in the lobby while the image comes: its seat lines read the roster the host has already sent,
			// here on the thread that pumps its session.
			if (!m_IsHost && config.persistentWorld && m_ReconnectClient.GetRosterReplica().HasRoster()) RefreshSeatViewsLocked(AdmissionNowMs());
			// Its own seat is the one its ticket names: a world's seats and the lobby's ids are not the same numbers.
			m_WorldJoinerSeatPeer = 0;
			if (!m_IsHost && config.persistentWorld && m_ReconnectClient.HasRecord()) {
				for (const NetH4Seat& entry: NetH4BuildSeatTable(config))
					if (!entry.cpu && entry.stableSeat == m_ReconnectClient.GetRecord().stableSeat) m_WorldJoinerSeatPeer = entry.lockstepPeerId;
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
		if (started && !AttachAdmissionPlane(*session, request, runnerConfig.matchConfig, runnerConfig.sessionConfig, manifest, &error)) {
			started = false;
		}
		NetResyncState resumeState;
		if (started && request.resumeConfig && request.resumeTick != 0) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			// The seats, credentials and bans the match had when it died, admitted again by the very
			// host that sealed them; the transports are empty because nobody has reconnected yet.
			if (!m_ReconnectHost.ImportMigrationState(m_ResumeAdmissionState, m_SeatAuth, runnerConfig.matchConfig, m_LocalPeerId, {}, AdmissionNowMs())) {
				started = false;
				error = "the stored admission state does not fit this match";
			} else {
				m_ReconnectHost.SetLiveMatch(false);
				resumeState = BuildResumeState(runnerConfig.matchConfig, request.resumeTick, m_ResumeRoundId, m_ResumeMatchId, m_ResumeSideState);
				runnerConfig.resumeMatchId = m_ResumeMatchId;
				runnerConfig.resumeTick = m_ResumeTick;
				runnerConfig.resumeDigest = m_ResumeArchiveDigest;
				runnerConfig.resumeSideStateHash = HashSideState(m_ResumeSideState);
			}
		}
		if (started && request.resumeConfig && request.resumeTick != 0) {
			// The checkpoint's own bytes are the lobby's state: a peer that lacks the archive receives
			// exactly the file the host is about to load.
			const std::filesystem::path archivePath = AutosaveStore::ArchivePath(runnerConfig.resumeMatchId, runnerConfig.resumeTick);
			std::ifstream in(archivePath, std::ios::binary);
			std::vector<uint8_t> archive;
			if (in) archive.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			std::vector<uint8_t> envelope;
			if (archive.empty()) {
				started = false;
				error = "the checkpoint archive is unreadable: " + archivePath.string();
			} else if (!NetResyncCodec::Encode(resumeState, archive, envelope, &error)) {
				started = false;
			} else {
				runner->SetStateToStream(std::move(envelope));
			}
		}
		runnerConfig.enableMigration = s_AdmissionEnabled && !request.dedicated && !runnerConfig.matchConfig.persistentWorld && GetNetAuthCrypto().IsRealCrypto();
		runnerConfig.migrationListenAddrs = NetLockstepCoordinator::MigrationListenAddrs(NetLanDiscovery::GetPrimaryLocalAddress(), false);
		{
			std::lock_guard<std::mutex> iceLock(m_MigrationIceMutex);
			m_MigrationIce.route = false;
		}
		runnerConfig.sealMigration = [this](uint8_t peer, const NetHash32& hash, std::vector<uint8_t>& sealed) { return SealMigrationCapsule(peer, hash, sealed); };
		runnerConfig.openMigration = [this](const NetLobbyMigration& capsule) { return OpenMigrationCapsule(capsule); };
		runnerConfig.configureMigration = [this](NetLockstepConfig& config) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (config.matchConfig.successorOrder.empty())
				return;
			config.migrationKey = m_MigrationKey;
			config.migrationGeneration = m_MigrationGeneration;
			bool ice = false;
			{
				std::lock_guard<std::mutex> iceLock(m_MigrationIceMutex);
				ice = m_MigrationIce.route;
			}
			config.migrationTransportFactory = [this, ice] { return MakeMigrationTransport(ice); };
			if (ice) {
				config.migrationIceDial = [this](INetTransport& transport, const std::string& identity, std::string* error) { return DialMigrationIce(transport, identity, error); };
				config.migrationIceHost = [this](INetTransport& listener) { HostMigrationIce(listener); };
			}
			if (m_MigrationAuthority != 0)
				config.authorityPeerId = m_MigrationAuthority;
			if (!m_MigrationMembers.empty())
				config.activePeerIds = m_MigrationMembers;
			if (m_IsHost && m_ChatSession) {
				config.activePeerIds = {static_cast<uint8_t>(m_ChatSession->GetLocalPeerId() + 1)};
				for (const auto& peer: m_ChatSession->GetReadyPeers())
					config.activePeerIds.push_back(static_cast<uint8_t>(peer.assignedPeerId + 1));
			}
		};

		NetIceJoinTarget iceTarget;
		bool iceSetupFailed = false;
		bool noDirectRoute = false;
		if (started && (iceWanted || resolveDirectory)) {
			started = SetUpIceTransport(request, manifest, *mux, runnerConfig.sessionConfig, runnerConfig.joinAddress, iceTarget, &error);
			iceSetupFailed = !started && !request.host && NetIcePrefersP2P(iceTarget, iceWanted);
#ifdef CCCP_WITH_GNS
			// A match whose links run through the directory hands over through it too: the successor takes its host end.
			if (started && m_Dispatcher) {
				runnerConfig.migrationListenAddrs = NetLockstepCoordinator::MigrationListenAddrs(runnerConfig.migrationListenAddrs.front(), true);
				std::lock_guard<std::mutex> iceLock(m_MigrationIceMutex);
				m_MigrationIce.route = true;
			}
			if (started && m_Dispatcher) {
				GnsDirectorySignalDispatcher* dispatcher = m_Dispatcher.get();
				mux->SetPump([this, dispatcher, p2p = mux->P2PGns(), previous = NetRelayConfig{},
				              initial = request.host ? mux->HostP2PConfig() : mux->GetJoinSpec().p2p,
				              personal = !g_SettingsMan.GetNetworkPlayerTurnServers().empty() || g_SettingsMan.HasNetworkTurnServersOverride()]() mutable {
					dispatcher->Update(SteadyNowMs());
					const auto snapshot = m_RelaySnapshot.load();
					NetRelayConfig offer = snapshot ? *snapshot : NetRelayConfig{};
					if (offer != previous) {
						GnsP2PConfig update = initial;
						if (!personal && initial.connectionMode != 1) {
							update.turnServerList.clear(); update.turnUserList.clear(); update.turnPassList.clear();
							update.relayOffer = "none";
							if (offer.Usable(UnixNowMs(nullptr) / 1000)) {
								offer.UdpLists(update.turnServerList, update.turnUserList, update.turnPassList);
								update.relayOffer = RelayOfferName(offer);
							}
							update.iceEnable = initial.connectionMode == 2 ? 1 : (initial.stunServerList.empty() ? 2 : 6) | (update.turnServerList.empty() ? 0 : 1);
						}
						p2p->UpdateListenerIceServers(update);
						previous = std::move(offer);
					}
				});
			}
#endif
		}
		if (request.rejoin) {
			// A returning seat's fresh session talks to the host live until its handshake is done.
			System::PrintDiagnosticLine("[net-match] rejoin phase Active -> Connecting");
			session->SetRejoinPhase(NetSession::RejoinPhase::Connecting);
			// It loads a checkpoint only if the host offers one on this connection: the round's opening offer may be retired.
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_ResumeHeldMatchId.clear();
			m_ResumeHeldTick = 0;
			m_ResumeHeldRound = 0;
		}
		if (started || iceSetupFailed) {
			if (request.host) ReadRelayOffer(runnerConfig.matchConfig.relay);
			started = StartLobbyConnection(mux, *transport, *session, *coordinator, *runner, runnerConfig, iceTarget, started, noDirectRoute, &error);
		}
		// A joiner whose lobby round carried a match state is RECONNECTING into a live match; it
		// launches from the received snapshot instead of a fresh activity. Launching a FRESH match
		// while the others play the snapshot would desync instantly, so a failed write fails the join.
		std::string pendingLoad;
		std::optional<NetResyncState> pendingState;
		std::optional<PendingAutosaveLoad> pendingAutosave;
		std::vector<uint8_t> pendingRoundStartScripts;
		if (started && request.resumeConfig && request.resumeTick != 0) {
			// The host's world comes out of its own store, never off the wire it just streamed.
			pendingAutosave = PendingAutosaveLoad{runnerConfig.resumeMatchId, runnerConfig.resumeTick};
			pendingState = resumeState;
			// The lobby callback that answers a resume offer runs on this thread, so what it recorded
			// is this thread's to read.
		} else if (started && !m_ResumeHeldMatchId.empty()) {
			// A client that answered the host's offer with its own copy loads that copy and derives the
			// same lockstep state the host put in the envelope it was spared.
			pendingAutosave = PendingAutosaveLoad{m_ResumeHeldMatchId, m_ResumeHeldTick};
			pendingState = BuildResumeState(runner->GetMatchConfig(), m_ResumeHeldTick, m_ResumeHeldRound, m_ResumeHeldMatchId, m_ResumeHeldSideState);
		}
		if (started && !pendingAutosave) {
			std::vector<uint8_t> receivedState = runner->TakeReceivedState();
			if (IsRoundStartScriptBlob(receivedState)) {
				pendingRoundStartScripts = std::move(receivedState);
			} else if (!receivedState.empty()) {
				if (IsWorldJoinImageBlob(receivedState)) {
					started = PrepareReceivedWorldJoin(receivedState, runner->GetMatchConfig(), pendingLoad, &error);
					if (started) NoteWorldCatchUpArmed(*session);
				} else {
					NetResyncState state;
					started = PrepareReceivedResync(receivedState, *coordinator, pendingLoad, state, &error);
					if (started) pendingState = std::move(state);
					// Staging the snapshot parked this thread, and the game thread cannot reach a session
					// the worker owns: the silence windows start again here, as they do for a catch-up.
					session->NotePumpParked();
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
				m_PendingAutosaveLoad = pendingAutosave;
				m_PendingResyncState = std::move(pendingState);
				m_PendingRoundStartScripts = request.host ? m_RoundStartScripts : std::move(pendingRoundStartScripts);
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
				// A seat told its round ended while it was held or rejoining completes on the host's end record,
				// its session kept for the rematch lobby that follows.
				if (const auto record = !request.host && m_Runner && m_Session && m_Session->IsReady() ? m_Runner->GetLobbySession().GetRoundEndedRecord() : std::nullopt) {
					const uint8_t localLockstepId = static_cast<uint8_t>(m_Session->GetLocalPeerId() + 1);
					for (const NetMatchPlayerSlot& slot : m_Runner->GetMatchConfig().players)
						if (slot.peerId == localLockstepId) m_LocalTeam = slot.team;
					m_LocalPeerId = localLockstepId;
					m_RoundEndRecord = *record;
					m_ReceivedEndWinner = RoundEndedWinnerTeam(*record);
					m_HostGoodbyeSeen = true;
					m_CompletedRoundFinalFrame = RoundEndedFinalFrame(*record);
					for (const NetTransportEvent& event: m_Runner->GetLobbySession().TakeEventsAfterRoundEnded()) QueueLobbyEvent(event);
					NetMatchSummary summary;
					summary.winnerTeam = RoundEndedWinnerTeam(*record);
					summary.result = RoundEndResultText(summary.winnerTeam, m_LocalTeam);
					summary.runningTicks = m_CompletedRoundFinalFrame;
					for (const NetMatchPlayerSlot& slot : m_Runner->GetMatchConfig().players)
						if (!slot.cpu) summary.peers.push_back(NetMatchSummary::Peer{slot.peerId, slot.displayName, slot.team});
					m_LastMatchSummary = summary;
					m_State = NetMatchServiceState::Completed;
					m_CompletedLobbySinceMs = SteadyNowMs();
					m_StatusText = summary.result;
					m_ErrorText.clear();
					System::PrintDiagnosticLine("[net-match] end record received final=" + std::to_string(m_CompletedRoundFinalFrame) +
					                            " winner_team=" + std::to_string(RoundEndedWinnerTeam(*record)) + " local_team=" + std::to_string(m_LocalTeam));
					m_WorkerDone = true;
					return;
				}
				m_State = NetMatchServiceState::Failed;
				// A start that died with the host's session is the departure itself, not a start fault; a host that
				// released this player's seat is still there.
				const bool seatReleased = m_Session && m_Session->HasReject() && m_Session->GetRejectReason() == NetRejectReason::SeatReleased;
				// A session that never heard the host never reached it: its setup error is the answer, not a departure.
				const bool reachedHost = m_Session && m_Session->GetStats().receivedMessages > 0;
				const bool lostHost = !request.host && !seatReleased &&
				    (m_Runner->DidLoseHostDuringSetup() || (reachedHost && ClientSessionLossIsHostDeparture(*m_Session)));
				System::PrintDiagnosticLine("[net-match] setup failed: " + error + (lostHost ? " (the host left)" : ""));
				// A player coming into a running match whose host went while the others play on: the match is changing host.
				// An application waiting on a held seat is one: such a seat is in a match with other players.
				NetReconnectClient* replica = m_Session ? m_Session->GetReconnectClient() : nullptr;
				const bool joiningWorld = m_Runner->SawWorldImageTransfer() && replica && replica->GetRosterReplica().HasRoster() &&
				                          MatchPlaysOnUnderANewHost(replica->GetRosterReplica().Roster());
				const bool applying = replica && replica->HasUnansweredApplication();
				const bool changingHost = lostHost && !NoteHostGoodbyeLocked(m_Session.get()) && (joiningWorld || applying);
				if (changingHost) {
					System::PrintDiagnosticLine(std::string("[net-match] the host left while this player was ") + (applying ? "applying for a seat" : "joining") +
					                            "; the others play on under a new host");
					if (applying) replica->CarryApplicationToNextHost();
				}
				m_StatusText = changingHost ? std::string(c_NetMatchChangingHostLine) : lostHost ? "The host left the match"
				                        : SetupFailureStatus(m_Session.get(), noDirectRoute, (m_RelayAttempted && noDirectRoute) || error.starts_with("Relay "));
				m_ErrorText = lostHost ? m_StatusText : (m_Session && m_Session->HasReject() ? m_Session->BuildPlayerRefusalText() : error);
				// A refusal that says the round is over is an answer, not a lost link: the seat completes.
				(void)NoteHostGoodbyeLocked(m_Session.get());
				// §9b: the refusals a joiner can answer - a running match, its own seat held for it, a world's slots all held.
				m_JoinRefusalKey = !request.host && m_Session && m_Session->HasReject() ? m_Session->GetMismatchKey() : std::string();
				m_JoinRefusalSeat.reset();
				if (const std::string seat = m_Session ? m_Session->GetMismatchExpected() : std::string(); m_JoinRefusalKey == "seat_held_for_you" && !seat.empty()) {
					m_JoinRefusalSeat = static_cast<uint16_t>(std::strtoul(seat.c_str(), nullptr, 10));
				}
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

	void NetMatchService::SetHostBanStorePath(std::string path) {
		s_HostBanStorePath = std::move(path);
	}

	void NetMatchService::SetJoinWaitPath(std::string path) {
		s_JoinWaitPath = std::move(path);
	}

	bool NetMatchService::WaitForJoinTrigger(const std::string& path, uint64_t budgetMs, std::string* error) {
		if (path.empty()) return true;
		const std::filesystem::path trigger(path);
		const auto opened = std::chrono::steady_clock::now();
		{
			std::ostringstream line;
			line << "[net-join-wait] waiting for " << path;
			System::PrintDiagnosticLine(line.str());
		}
		for (;;) {
			std::error_code fsError;
			if (std::filesystem::exists(trigger, fsError) && !fsError) {
				const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - opened).count();
				{
					std::ostringstream line;
					line << "[net-join-wait] released after " << waitedMs << "ms";
					System::PrintDiagnosticLine(line.str());
				}
				return true;
			}
			const uint64_t elapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - opened).count());
			if (elapsedMs >= budgetMs) break;
			const uint64_t remainingMs = budgetMs - elapsedMs;
			std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint64_t>(c_JoinWaitPollMs, remainingMs)));
		}
		if (error) *error = "join wait timed out: " + path + " did not appear within " + std::to_string(budgetMs) + "ms";
		{
			std::ostringstream line;
			line << "[net-join-wait] timed out after " << budgetMs << "ms waiting for " << path;
			System::PrintDiagnosticErrorLine(line.str());
		}
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
		// A round that has already ended for a relaunch keeps its end: the relaunch completes for the others and the
		// removed seat is gone from the round it starts.
		if (m_Coordinator && m_State == NetMatchServiceState::Running && m_Coordinator->IsRunning() && !m_Coordinator->HasPendingRecoveryStop() &&
		    m_LastRemovalIssue.lockstepPeerId != 0) {
			const char* why = action == NetParticipantRemovalAction::Kick ? "removed from the session" : "banned from the session";
			m_Coordinator->EvictRemovedPeer(m_LastRemovalIssue.lockstepPeerId, why, nowMs);
		}
		if (m_LastRemovalIssue.connection != c_InvalidNetPeerId) {
			const NetRejectReason reason = action == NetParticipantRemovalAction::Kick ? NetRejectReason::ParticipantRemoved : NetRejectReason::ParticipantBanned;
			const char* text = action == NetParticipantRemovalAction::Kick ? c_NetRemovedLinkText : c_NetBannedLinkText;
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
		config.pumpHost = [this](NetSession& session) {
			DrainPendingModeration(session);
			// The panel's rows come from here while the worker owns the plane: after the drain, so a
			// removed seat has stopped being a row the host can act on by the time the result is read.
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (ActiveWireLocked()) PublishRelayOfferLocked(session, *ActiveWireLocked());
			PublishLobbyModerationViewLocked();
		};
	}

	void NetMatchService::DrainPendingModeration(NetSession& session) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_IsHost) {
			m_PendingModeration.clear();
			return;
		}
		if (m_PendingModeration.empty() || m_State != NetMatchServiceState::Starting || m_ResyncHealOpen) {
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
		// The Seats panel calls this inside the plane's window, and a removal reads the round and evicts from it.
		NetLockstepPlane::Gap plane("seat removal");
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
		if (RelaunchInFlightLocked()) {
			// A relaunch completes for everyone it was started for; the removal lands on the round it opens.
			return QueueModerationLocked(PendingModeration{false, selection, action, {}});
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
		// The ban list's Unban runs inside the plane's window too, and asks whether a relaunch holds the round.
		NetLockstepPlane::Gap plane("unban");
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_IsHost) {
			m_LastKickBanResult = NetKickBanResult::NotHosting;
			return m_LastKickBanResult;
		}
		if (m_State == NetMatchServiceState::Starting || RelaunchInFlightLocked()) {
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
			{
				std::ostringstream line;
				line << "[net-reconnect] moderation: substitute seat " << seat.stableSeat << " -> "
			          << NetH4ModerationResultName(result);
				System::PrintDiagnosticLine(line.str());
			}
			if (result == NetH4ModerationResult::Ok && s_AutoSubstituteThenCancel) {
				const NetH4ModerationResult cancelled = m_ReconnectHost.CancelSubstitution(seat.stableSeat, nowMs);
				{
					std::ostringstream line;
					line << "[net-reconnect] moderation: cancel seat " << seat.stableSeat << " -> "
				          << NetH4ModerationResultName(cancelled);
					System::PrintDiagnosticLine(line.str());
				}
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

	bool NetMatchService::AttachAdmissionPlane(NetSession& session, const NetMatchServiceRequest& request, const NetMatchConfig& matchConfig, const NetSessionConfig& sessionConfig, const NetIdentityManifest& manifest, std::string* error) {
		m_AdmissionAttached = false;
		if (!s_AdmissionEnabled) {
			return true;
		}
		const NetH4Identity identity = BuildH4Identity(manifest);
		if (request.host) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (!m_SeatAuth.IsActive()) {
				// Fail closed: with no crypto nothing can be issued or proven, so the session keeps the
				// pre-admission handshake rather than gating every join on a ticket it cannot mint.
				return true;
			}
			// A ban list that is on disk but unreadable would admit every identity it names, so the host
			// is told which file and hosts nothing until that file loads.
			const std::string banPath = s_HostBanStorePath.empty() ? NetHostBanStore::DefaultPath() : s_HostBanStorePath;
			m_BanStore.SetPath(banPath);
			std::string banError;
			if (!m_BanStore.Load(&banError)) {
				if (error) *error = banError + ": " + banPath;
				return false;
			}
			m_ReconnectHost.Configure(&m_SeatAuth, sessionConfig.sessionId, identity);
			m_ReconnectHost.SetSeatTable(NetH4BuildSeatTable(matchConfig), matchConfig.mode);
			m_ReconnectHost.SetHostAddress(NetLanDiscovery::GetPrimaryLocalAddress());
			m_ReconnectHost.SetMatchConfigHash(NetMatchConfigUtil::HashConfig(matchConfig));
			m_ReconnectHost.SetLiveMatch(false);
			m_ReconnectHost.SetPersistentWorld(matchConfig.persistentWorld);
			m_ReconnectHost.SetDropOwnershipSource(&NetMatchService::CollectDropOwnership, this);
			m_ReconnectHost.SetSeatSimIdentitySource(&NetMatchService::SeatSimIdentitySource, this);
			session.SetReconnectHost(&m_ReconnectHost);
			session.EnableParticipantProof(nullptr);
			m_ReconnectHost.SetBanStore(&m_BanStore);
			session.SetHostBanStore(&m_BanStore);
			m_AdmissionAttached = true;
			return true;
		}
		m_TicketStore.SetPath(s_TicketStorePath.empty() ? NetReconnectTicketStore::DefaultPath() : s_TicketStorePath);
		m_ParticipantStore.SetPath(NetParticipantIdentityStore::DefaultPath());
		(void)m_ParticipantStore.LoadOrCreate(nullptr);
		m_ReconnectClient.Configure(&m_TicketStore, identity, request.playerName.empty() ? "Client" : request.playerName);
		m_ReconnectClient.SetUnixClock(&UnixNowMs, nullptr);
		// The record names the host it belongs to; the config hash is context, not a gate - a client
		// adopts the host's match config in the lobby round that follows.
		// The record keeps the host's port beside its address, so a rejoin finds a host on any port.
		const bool bareAddress = !request.address.empty() && request.address.find(':') == std::string::npos;
		m_ReconnectClient.SetHostContext(bareAddress && request.port != 0 ? request.address + ":" + std::to_string(request.port) : request.address, NetHash32{});
		m_ReconnectClient.SetWorldTarget(request.persistentWorld || matchConfig.persistentWorld);
		m_ReconnectClient.SetDirectorySessionId(request.sessionId);
		// A rejoin holds its seat already: it reclaims, it never applies again.
		m_ReconnectClient.SetApplyForSeat((s_ApplyForSeat || s_ApplyOnce) && !request.rejoin, s_ApplyOnce ? s_ApplyOnceSeat : s_ApplySeat);
		s_ApplyOnce = false;
		s_ApplyOnceSeat = c_NetH4AnySubstitutableSeat;
		session.SetReconnectClient(&m_ReconnectClient);
		session.EnableParticipantProof(&m_ParticipantStore);
		m_AdmissionAttached = true;
		return true;
	}

	void NetMatchService::ScanStoredTicket() {
		if (!s_AdmissionEnabled) {
			m_ReconnectUx.DismissOffer();
			m_ReconnectUx.StopWatchingForHostReturn();
			return;
		}
		m_TicketStore.SetPath(s_TicketStorePath.empty() ? NetReconnectTicketStore::DefaultPath() : s_TicketStorePath);
		NetH4TicketRecord record;
		const NetH4TicketLoadResult load = m_TicketStore.Load(UnixNowMs(nullptr), record, nullptr);
		m_ReconnectUx.OfferStoredTicket(load, record.hostAddress);
		// 7e: the ticket names a match whose host is not here. The prompt waits for that host to come
		// back rather than failing a rejoin at a host that is gone.
		if (load == NetH4TicketLoadResult::Loaded) {
			m_ReconnectUx.WatchForHostReturn(record.hostAddress, record.directorySessionId);
		} else {
			m_ReconnectUx.StopWatchingForHostReturn();
		}
	}

	void NetMatchService::PumpHostReturnWatch(uint64_t nowMs) {
		if (!m_ReconnectUx.IsAwaitingHostReturn()) {
			m_ReturnWatch.StopBrowsing();
			return;
		}
		const std::string sessionId = m_ReconnectUx.GetWatchedSessionId();
		const std::string baseUrl = g_SettingsMan.GetSessionDirectoryUrl();
		if (sessionId.empty() || baseUrl.empty()) {
			// Without a directory there is nothing to watch: the prompt says so and offers the address.
			m_ReconnectUx.NoteHostUnwatchable("there is no directory to watch");
			return;
		}
		if (!m_ReturnWatchConfigured) {
			m_ReturnWatch.Configure(baseUrl, g_SettingsMan.GetSessionDirectoryInstallKey(), g_SettingsMan.GetSessionDirectoryCertSha256());
			m_ReturnWatchConfigured = true;
		}
		m_ReturnWatch.PollList(nowMs);
		if (m_ReturnWatch.ListReplies() == 0) {
			// A directory that cannot be reached must not hold the prompt shut: the player keeps the
			// address route while the listing is unavailable.
			if (!m_ReturnWatch.ListError().empty()) m_ReconnectUx.NoteHostUnwatchable("the directory is unreachable");
			return;
		}
		// The row must be the same session, listed as a lobby or a running match.
		const auto& rows = m_ReturnWatch.Rows();
		m_ReconnectUx.NoteHostReturn(std::any_of(rows.begin(), rows.end(), [&](const NetDirectorySessionRow& row) {
			return NetDirectoryRowIsWatchedHost(row, sessionId);
		}));
	}

	NetMatchServiceRequest NetMatchService::BuildTicketRejoinRequest(const NetH4TicketRecord& record, const std::string& playerName, bool liveWorldTarget) {
		NetMatchServiceRequest request;
		request.host = false;
		request.address = record.hostAddress;
		request.sessionId = record.directorySessionId;
		request.playerName = playerName.empty() ? "Client" : playerName;
		request.resyncOnDesync = true;
		request.rejoin = true;
		// The ticket's own flag survives a relaunch, which the live flags do not.
		request.persistentWorld = record.persistentWorld || liveWorldTarget;
		if (request.persistentWorld) {
			request.activityPreset = "Persistent World";
		}
		return request;
	}

	NetMatchServiceRequest NetMatchService::BuildHeldRejoinRequest(const NetH4TicketRecord& record, const std::string& playerName, bool liveWorldTarget, const NetMatchServiceRequest& liveRoute) {
		NetMatchServiceRequest request = BuildTicketRejoinRequest(record, playerName, liveWorldTarget);
		if (!liveRoute.host && (!liveRoute.address.empty() || !liveRoute.sessionId.empty())) {
			request.address = liveRoute.address;
			request.sessionId = liveRoute.sessionId;
			request.port = liveRoute.port;
		}
		return request;
	}

	bool NetMatchService::BeginHeldRejoin(std::string* error) {
		const uint64_t prior = m_Coordinator ? m_Coordinator->SentInputThrough() : 0;
		auto liveRoute = m_LastJoinRoute;
		// A host whose match went on under a successor returns to that successor as a player, with its own seat's ticket.
		if (m_Coordinator && m_Coordinator->SupersedingPeer() != 0) {
			const NetMatchConfig& config = m_Coordinator->GetConfig().matchConfig;
			const auto endpoint = std::find_if(config.migrationPeers.begin(), config.migrationPeers.end(), [&](const auto& candidate) { return candidate.peerId == m_Coordinator->SupersedingPeer(); });
			if (endpoint != config.migrationPeers.end() && endpoint->listenPort != 0 && !endpoint->listenAddrs.empty()) {
				NetMatchServiceRequest route;
				route.host = false;
				route.address = endpoint->listenAddrs.front();
				route.port = endpoint->listenPort;
				liveRoute = route;
			}
		}
		// The match published who hosts it next; a seat that finds its host gone asks them in that order.
		m_HeldRejoinRoutes.clear();
		m_ReconnectRouteTurn = 0;
		m_HeldRejoinPriorInput = prior;
		if (m_Coordinator) {
			const NetMatchConfig& config = m_Coordinator->GetConfig().matchConfig;
			const uint8_t host = m_Coordinator->GetHostPeerId();
			// A successor still finishing its handover is asked a second time.
			for (int pass = 0; pass < 2; ++pass) {
				for (const uint8_t peer: config.successorOrder) {
					if (peer == host || peer == m_LocalPeerId) continue;
					const auto endpoint = std::find_if(config.migrationPeers.begin(), config.migrationPeers.end(), [&](const auto& candidate) { return candidate.peerId == peer; });
					if (endpoint == config.migrationPeers.end() || endpoint->listenPort == 0) continue;
					for (const std::string& address: endpoint->listenAddrs) {
						NetMatchServiceRequest route;
						route.host = false;
						route.address = address;
						route.port = endpoint->listenPort;
						m_HeldRejoinRoutes.push_back(std::move(route));
					}
				}
			}
		}
		m_LeaveExchangeRun = true;
		ScenarioRunner::DiscardHeldLocalInputs();
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_HeldRejoinDriving = true;
			m_HeldRejoinFailedAttempts = 0;
			m_HeldRejoinRetryAtMs = 0;
		}
		const bool started = BeginTicketRejoinOnRoute(error, liveRoute ? &*liveRoute : nullptr);
		if (started) ScenarioRunner::SetWorldCatchUpPriorInputThrough(prior);
		return started;
	}

	void NetMatchService::EndRoundCatchUpLocked() {
		if (!m_WorldCatchUp.active && !m_InPlaceCatchUp) return;
		m_CatchUpCoordinator.reset();
		m_ActivateCatchUpLocalSeat = {};
		m_InPlaceCatchUp = false;
		m_WorldCatchUp = {};
		ScenarioRunner::ReleaseWorldCatchUp();
	}

	void NetMatchService::EndHeldRejoinInNextRound() {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_HeldRejoinDriving = false;
		m_HeldRejoinFailedAttempts = 0;
		m_HeldRejoinRetryAtMs = 0;
		m_HeldRejoinRoutes.clear();
		m_HeldRejoinPriorInput = 0;
		// The rejoin is over: its phase, and the ceiling that phase keeps, end with it.
		SetRejoinPhaseLocked(NetSession::RejoinPhase::Active);
		// The input this seat sent belongs to the round that ended; the new round fences none of it.
		ScenarioRunner::SetWorldCatchUpPriorInputThrough(0);
	}

	bool NetMatchService::RematchLossReturnsThroughRejoin(bool isHost, bool hostEndedMatch, bool rosterRefused, bool sessionReady, bool hasReject, NetRejectReason reason, bool linkLost,
	                                                      bool startNeverCame, bool heldAtStart) {
		if (!isHost && !hostEndedMatch && heldAtStart) return true;
		// A start that never came on a link this side still reads ready is the host playing the round with this seat held.
		if (isHost || hostEndedMatch || rosterRefused || (sessionReady && !startNeverCame) || !linkLost) return false;
		// A link closed with no reason from the host, or lost in transport, is a drop: the host keeps the seat for its return.
		return !hasReject || reason == NetRejectReason::InternalError || reason == NetRejectReason::HostLinkLost || reason == NetRejectReason::Timeout;
	}

	bool NetMatchService::RematchReturnOwed() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_RematchReturnOwed;
	}

	bool NetMatchService::HeldRejoinRetriesTheHost(bool lostDuringSetup, bool hasReject, NetRejectReason reason, const std::string& rejectSummary) {
		return !lostDuringSetup && hasReject && reason == NetRejectReason::Timeout && rejectSummary == "client hello timeout";
	}

	NetMatchService::HeldRejoinStep NetMatchService::NextHeldRejoinStep(uint8_t failedAttempts, bool hasReject, NetRejectReason reason, const std::string& rejectText) {
		HeldRejoinStep step;
		// The host's final word on the seat ends the rejoin with that word.
		const bool final = hasReject && (reason == NetRejectReason::SeatReassigned || reason == NetRejectReason::ParticipantRemoved || reason == NetRejectReason::ParticipantBanned ||
		                                 reason == NetRejectReason::SeatReleased || reason == NetRejectReason::SessionEnded || reason == NetRejectReason::IdentityUnproven ||
		                                 (reason >= NetRejectReason::ProtocolMismatch && reason <= NetRejectReason::UserdataModulesNotAllowed));
		if (final) {
			step.stop = rejectText.empty() ? "Could not rejoin - the host refused the seat" : rejectText;
			return step;
		}
		if (failedAttempts >= c_RosterReturnAttempts) {
			step.stop = "Could not rejoin - the host did not take the seat back after " + std::to_string(failedAttempts) + " tries";
			return step;
		}
		step.retry = true;
		step.delayMs = static_cast<uint32_t>(c_RosterReturnBackoffMs << (failedAttempts > 0 ? failedAttempts - 1 : 0));
		return step;
	}

	bool NetMatchService::BeginHeldRejoinOnNextHost(std::string* error) {
		HeldRejoinStep step;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_IsHost) return false;
			const bool lostDuringSetup = m_Runner && m_Runner->DidLoseHostDuringSetup();
			const bool hasReject = m_Session && m_Session->HasReject();
			const NetRejectReason reason = hasReject ? m_Session->GetRejectReason() : NetRejectReason::InternalError;
			// A host that did not answer one dial is not gone; only a host that is gone sends the seat on.
			const bool helloUnanswered = m_Session && HeldRejoinRetriesTheHost(lostDuringSetup, hasReject, reason, m_Session->GetRejectSummary());
			const bool hostGone = !helloUnanswered && (lostDuringSetup || (m_Session && ClientSessionLossIsHostDeparture(*m_Session)));
			if (hostGone) {
				// A host that answered the round is over is not gone: the seat completes on what it holds.
				if (NoteHostGoodbyeLocked(m_Session.get()) || m_HeldRejoinRoutes.empty()) return false;
			} else {
				step = NextHeldRejoinStep(++m_HeldRejoinFailedAttempts, hasReject, reason, hasReject ? m_Session->BuildRejectText() : std::string());
				if (!step.retry) {
					m_HeldRejoinRetryAtMs = 0;
					m_StatusText = step.stop;
					if (error) *error = step.stop;
					return false;
				}
				m_HeldRejoinRetryAtMs = SteadyNowMs() + step.delayMs;
				m_StatusText = "Could not rejoin - retrying";
			}
		}
		if (step.retry) {
			System::PrintDiagnosticLine("[net-match] held rejoin: attempt " + std::to_string(m_HeldRejoinFailedAttempts) + " failed with the host still there; asking it again in " +
			                            std::to_string(step.delayMs) + " ms");
			return true;
		}
		while (!m_HeldRejoinRoutes.empty()) {
			const NetMatchServiceRequest route = m_HeldRejoinRoutes.front();
			m_HeldRejoinRoutes.pop_front();
			System::PrintDiagnosticLine("[net-match] held rejoin: the host is gone; rejoining the successor at " + route.address + ":" + std::to_string(route.port));
			if (RejoinSuccessorRoute(route, error)) return true;
		}
		return false;
	}

	std::string NetMatchService::GetHostUnreachableLine() const {
		std::lock_guard<std::mutex> lock(m_Mutex);
		static const std::string c_Prefix = "PeerHeld:";
		return m_HeldUnreachableText.rfind(c_Prefix, 0) == 0 ? m_HeldUnreachableText.substr(c_Prefix.size()) : m_HeldUnreachableText;
	}

	void NetMatchService::LeaveHeldWait() {
		if (GetState() == NetMatchServiceState::Running) {
			LeaveMatch("Match left");
			return;
		}
		// A rejoin attempt may be in flight: it ends with the session, and the ticket on disk is the player's way back.
		Destroy();
		ScanStoredTicket();
	}

	bool NetMatchService::PumpHeldRejoin(std::string* error) {
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (m_HeldRejoinRetryAtMs == 0) return false;
			if (SteadyNowMs() < m_HeldRejoinRetryAtMs) return true;
			m_HeldRejoinRetryAtMs = 0;
		}
		// An attempt that cannot even start fails as an attempt does: the wait reads the failure and asks for the next step.
		return BeginTicketRejoin(error);
	}

	bool NetMatchService::RejoinSuccessorRoute(const NetMatchServiceRequest& route, std::string* error) {
		{
			// The ticket names the host its match is on; the successor hosts that match now, as a survivor's ticket says.
			std::lock_guard<std::mutex> lock(m_Mutex);
			NetH4TicketRecord record;
			m_TicketStore.SetPath(s_TicketStorePath.empty() ? NetReconnectTicketStore::DefaultPath() : s_TicketStorePath);
			if (m_TicketStore.Load(UnixNowMs(nullptr), record, nullptr) == NetH4TicketLoadResult::Loaded && record.hostAddress != route.address) {
				record.hostAddress = route.address;
				(void)m_TicketStore.Store(record, nullptr);
			}
		}
		m_LeaveExchangeRun = true;
		if (!BeginTicketRejoinOnRoute(error, &route)) return false;
		ScenarioRunner::SetWorldCatchUpPriorInputThrough(m_HeldRejoinPriorInput);
		return true;
	}

	bool NetMatchService::BeginTicketRejoin(std::string* error) {
		return BeginTicketRejoinOnRoute(error, nullptr);
	}

	bool NetMatchService::BeginTicketRejoinOnRoute(std::string* error, const NetMatchServiceRequest* liveRoute) {
		NetH4TicketRecord record;
		m_TicketStore.SetPath(s_TicketStorePath.empty() ? NetReconnectTicketStore::DefaultPath() : s_TicketStorePath);
		const NetH4TicketLoadResult load = m_TicketStore.Load(UnixNowMs(nullptr), record, nullptr);
		m_ReconnectUx.OfferStoredTicket(load, record.hostAddress);
		if (load != NetH4TicketLoadResult::Loaded) {
			if (error) *error = m_ReconnectUx.GetOfferText().empty() ? "no reconnect ticket to rejoin with" : m_ReconnectUx.GetOfferText();
			return false;
		}
		const bool world = m_MatchConfig.persistentWorld || m_LastJoinTargetPersistentWorld;
		return Start(liveRoute ? BuildHeldRejoinRequest(record, m_LocalName, world, *liveRoute) : BuildTicketRejoinRequest(record, m_LocalName, world), error);
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
			// The host authors the world's capacity; the preset's default spreads the seated humans the
			// way the mode does, so a host that names nothing still publishes an explicit contract.
			if (request.worldTeamCapacity.has_value()) {
				config.worldTeamCapacity = *request.worldTeamCapacity;
			} else {
				config.worldTeamCapacity = {};
				for (uint32_t seat = 0; seat < seatedHumans; ++seat) {
					const size_t team = mode == NetMatchMode::CoopPvE ? 0 : (seat % NetMatchConfigUtil::c_WorldTeamCount);
					++config.worldTeamCapacity[team];
				}
			}
			config.worldMaxSpectators = request.worldMaxSpectators.value_or(NetMatchConfigUtil::c_MaxWorldSpectators);
			// Test lever: a scripted world names its watcher bound; unset, nothing changes.
			if (const char* lever = std::getenv("CC_TEST_WORLD_MAX_SPECTATORS"); lever && *lever) {
				config.worldMaxSpectators = static_cast<uint8_t>(std::clamp(std::atoi(lever), 0, static_cast<int>(NetMatchConfigUtil::c_MaxWorldSpectators)));
			}
			config.worldRespawnDelaySeconds = request.worldRespawnDelaySeconds.value_or(NetMatchConfigUtil::c_DefaultWorldRespawnDelaySeconds);
			config.activityPreset = request.activityPreset.empty() ? "Persistent World" : request.activityPreset;
			config.peerCount = request.peerCount;
		} else {
			config.peerCount = humanCount == 0 ? 1 : request.peerCount;
		}
		config.dedicated = request.dedicated;
		// The host publishes the checkpoint cadence the whole match follows; a client's own setting never steers one.
		if (request.host) {
			// Every minute through every hour, or off; a run's own cadence override keeps its seconds.
			uint32_t seconds = request.autosaveSeconds.value_or(GetAutosaveSeconds());
			if (seconds != 0 && (request.autosaveSeconds.has_value() || !s_AutosaveSecondsOverridden)) {
				seconds = std::clamp(seconds, c_MinAutosaveIntervalSeconds, c_MaxAutosaveIntervalSeconds);
			} else {
				seconds = std::min(seconds, c_MaxAutosaveIntervalSeconds);
			}
			config.autosaveEnabled = seconds > 0;
			config.autosaveIntervalSeconds = seconds;
			// The rest of the host's saved session options ride the same config to every peer.
			config.delayPolicy = request.delayPolicy.value_or(config.delayPolicy);
			config.slowPlayerBoundTicks = request.slowPlayerBoundTicks.value_or(config.slowPlayerBoundTicks);
			// Only the default policy is offered, whatever a saved preset carries: the others do not yet keep a dropped player's seat.
			config.slowPlayerPolicy = NetSlowPlayerPolicy::Substitute;
			config.idleWaitMinutes = request.idleWaitMinutes.value_or(config.idleWaitMinutes);
			config.automaticRepair = request.automaticRepair.value_or(config.automaticRepair);
			config.pathHorizonTicks = request.pathHorizonTicks.value_or(config.pathHorizonTicks);
			config.frameRedundancyTicks = request.frameRedundancyTicks.value_or(config.frameRedundancyTicks);
			config.returnWindowMinutes = request.returnWindowMinutes.value_or(config.returnWindowMinutes);
		}
		// CPU teams follow human teams and consume no peer identity.
		config.players.clear();
		const uint8_t firstHumanPeer = request.dedicated ? 2 : 1;
		const uint32_t rosterHumans = world ? seatedHumans : humanCount;
		for (uint8_t peerId = firstHumanPeer; peerId < firstHumanPeer + rosterHumans; ++peerId) {
			NetMatchPlayerSlot slot;
			slot.peerId = peerId;
			slot.team = mode == NetMatchMode::CoopPvE ? 0 : static_cast<uint8_t>(peerId - firstHumanPeer);
			if (world) {
				// The world's slot table is spent in team order, so the roster names the same team for the
				// same lockstep id and an Activate binds the player slot the capacity gave it.
				uint32_t offset = peerId - firstHumanPeer;
				for (size_t team = 0; team < config.worldTeamCapacity.size(); ++team) {
					if (offset < config.worldTeamCapacity[team]) {
						slot.team = static_cast<uint8_t>(team);
						break;
					}
					offset -= config.worldTeamCapacity[team];
				}
			}
			slot.cpu = false;
			slot.displayName = peerId == config.hostPeerId ? PlayerNameOrDefault(request, true)
			                                               : NetMatchConfigUtil::UnseatedSlotName(peerId, world);
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
				slot.displayName = NetMatchConfigUtil::UnseatedSlotName(peerId, true);
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

	uint32_t NetMatchService::ImageReturnIncarnation(uint32_t ticket, uint32_t roundKnows) {
		return std::max(ticket, roundKnows + 1);
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
		if (!request.slowPlayerBoundTicks) request.slowPlayerBoundTicks = saved.slowPlayerBoundTicks;
		if (!request.slowPlayerPolicy) request.slowPlayerPolicy = saved.slowPlayerPolicy;
		if (!request.idleWaitMinutes) request.idleWaitMinutes = saved.idleWaitMinutes;
		if (!request.automaticRepair) request.automaticRepair = saved.automaticRepair;
		if (!request.pathHorizonTicks) request.pathHorizonTicks = saved.pathHorizonTicks;
		if (!request.returnWindowMinutes) request.returnWindowMinutes = saved.returnWindowMinutes;
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
