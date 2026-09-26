#pragma once

#include "NetDirectoryClient.h"
#include "NetLanDiscovery.h"
#include "NetLobbySnapshot.h"
#include "NetMatchReplay.h"
#include "NetMatchRunner.h"
#include "NetMuxTransport.h"
#include "NetHostBanStore.h"
#include "NetParticipantCrypto.h"
#include "NetReconnectSession.h"
#include "NetReconnectTicketStore.h"
#include "NetReconnectUx.h"
#include "NetSeatAuth.h"
#include "AutosaveStore.h"
#include "NetResyncState.h"
#include "ActivityMan.h"
#include "NetWorldJoin.h"
#include "Singleton.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define g_NetMatchService NetMatchService::Instance()

namespace RTE {

	class Activity;
	class LoopbackTransport;
	class GnsDirectorySignalDispatcher;
	class GnsTransport;
	class SettingsMan;

	enum class NetRejoinAnswer : uint8_t {
		Resync = 0,
		MatchOver = 1,
	};

	struct NetMatchE2ETickClock {
		uint64_t firstTick = UINT64_MAX;
		uint64_t lastTick = UINT64_MAX;
		uint64_t priorTicks = 0;
		uint64_t matchFirstFrame = UINT64_MAX;
		uint64_t segmentFirstFrame = 0; // The frame this segment resumed at; 0 when only the observed ticks are known.

		void NoteSimTick(uint64_t nowTick) {
			if (matchFirstFrame == UINT64_MAX) matchFirstFrame = nowTick;
			if (firstTick == UINT64_MAX) {
				firstTick = nowTick;
			}
			lastTick = nowTick;
		}
		uint64_t SegmentTicks() const {
			if (firstTick == UINT64_MAX) {
				return 0;
			}
			const uint64_t origin = segmentFirstFrame > 0 ? segmentFirstFrame : firstTick;
			return lastTick >= origin ? lastTick - origin + 1 : 0;
		}
		// A private return retains the original run budget, including disk-resumed rounds.
		void OnResyncRelaunch(uint64_t resumeFrame = 0) {
			if (matchFirstFrame == UINT64_MAX && resumeFrame > 0) matchFirstFrame = resumeFrame;
			priorTicks = resumeFrame > 0 ? (resumeFrame > matchFirstFrame ? resumeFrame - matchFirstFrame : 0) : priorTicks + SegmentTicks();
			segmentFirstFrame = resumeFrame;
			firstTick = UINT64_MAX;
			lastTick = UINT64_MAX;
		}
		void OnNewMatch() {
			priorTicks = 0;
			matchFirstFrame = UINT64_MAX;
			segmentFirstFrame = 0;
			firstTick = UINT64_MAX;
			lastTick = UINT64_MAX;
		}
		uint64_t Total() const { return priorTicks + SegmentTicks(); }
		bool EarlyOverIsSetupFailure() const { return Total() < 100; }
		bool EarlyOverIsSetupFailure(uint64_t matchTick) const { return matchTick < 100; }
	};

	inline uint64_t ParseLockstepStopTick(const std::string& error, uint64_t fallbackTick) {
		if (error.size() < 6 || error.compare(0, 5, "tick ") != 0) {
			return fallbackTick;
		}
		uint64_t tick = 0;
		bool any = false;
		for (size_t i = 5; i < error.size(); ++i) {
			const char c = error[i];
			if (c < '0' || c > '9') {
				break;
			}
			any = true;
			tick = tick * 10 + static_cast<uint64_t>(c - '0');
		}
		return any ? tick : fallbackTick;
	}

	// Cap completion is the match frame, not how long this process has run.
	inline bool NetMatchE2EReachedCap(uint64_t runningTicks, uint64_t matchTick, uint64_t cap) {
		(void)runningTicks;
		return matchTick >= cap;
	}

	// The stop a peer sends once the round has run its planned length.
	inline constexpr const char* c_NetMatchE2ECompleteStop = "Complete:e2e complete";

	/// Whether an e2e round's stop is the round reaching its planned end rather than a break.
	inline bool NetMatchE2ERoundReachedPlannedEnd(const std::string& error, uint64_t runningTicks, uint64_t matchTick, uint64_t cap) {
		// This stop originates from the peer that ran the round to its plan, so it ends the round on
		// every peer whatever tick the local sim is on when it lands.
		if (error.find(c_NetMatchE2ECompleteStop) != std::string::npos) {
			return true;
		}
		return NetMatchE2EReachedCap(runningTicks, matchTick, cap) &&
		       (error.find("Complete:") != std::string::npos ||
		        error.find("MissingFrameTimeout") != std::string::npos ||
		        error.find("PeerDisconnected") != std::string::npos);
	}

	/// Where a session-id join has to dial, once a directory row has been resolved.
	struct NetIceJoinTarget {
		std::string identity;  //!< The host's GNS identity; empty on an ip-only row.
		std::string joinMode;  //!< "ip" | "ice" | "either", as the row carries it.
		std::string address;   //!< Set when the row also advertises a direct address.
		uint16_t port = 0;
		bool persistentWorld = false;
	};

	/// The GNS identity a host binds for a directory session; the dispatcher's rule, readable in a
	/// build without GameNetworkingSockets.
	std::string NetIceHostIdentity(const std::string& sessionId);

	/// Reports ICE reachability only for the directory id bound to the listener.
	std::string NetIceRowJoinMode(bool iceEnabled, bool hasDirectAddress, const std::string& boundSessionId, const std::string& rowSessionId);
	/// Prefers NAT traversal when both the setting and the directory row allow it.
	bool NetIcePrefersP2P(const NetIceJoinTarget& target, bool iceEnabled);
	/// Keeps an Internet selection attached to its directory session.
	std::string NetIceMenuJoinAddress(const NetDirectoryClient::GameRow& row);

	/// Resolves a session id against a directory listing. Empty and a filled target when the row can
	/// be joined, else the join list's own refusal label for it.
	std::string NetIceResolveSessionRow(const std::vector<NetDirectorySessionRow>& rows, const NetDirectoryLocalIdentity& local, const std::string& sessionId, NetIceJoinTarget* out, const NetDirectoryLocalIdentity* worldLocal = nullptr, bool reservedSeat = false);

	enum class NetHostHandoverState { Live, HostLost, Migrating };

	enum class NetMatchServiceState {
		Idle,
		Starting,
		ReadyToLaunch,
		Running,
		Completed,
		Failed,
	};

	/// One compatible scene for the host's activity picker, in scene-manager order.
	struct NetHostSceneChoice {
		std::string name;
		std::string module;
	};

	/// A GameActivity the host picker may list: not a test, and at least one compatible loaded scene.
	struct NetHostActivityChoice {
		std::string preset;
		std::string module;
		std::string activityType;
		std::vector<NetHostSceneChoice> scenes;
	};

	struct NetMatchServiceRequest {
		bool host = false;
		std::string address = "127.0.0.1";
		uint16_t port = 41010;
		std::string playerName = "Player";
		std::string activityPreset = "Skirmish Defense";
		std::string activityModule; // The module that defines the preset; empty resolves to the module defining it.
		std::string activityType; // Empty keeps MakeDefault's GAScripted.
		std::string sceneName; // Empty resolves to the first compatible scene (Grasslands when the activity allows it).
		std::string sceneModule;
		std::optional<NetMatchStandardRules> standardRules;
		NetActorOwnershipPolicy ownershipPolicy = NetActorOwnershipPolicy::TeamOwner;
		uint16_t inputDelayFrames = 0; // Lockstep input-delay buffer; the host picks it, the client agrees at the start handshake.
		bool autoInputDelay = false; // Host: raise the delay to cover the measured peer RTT (the manual value stays the floor).
		uint8_t peerCount = 2; // Connected peers (2..4), including a dedicated host.
		std::optional<uint32_t> humans; // Omitted seats every available human peer.
		std::optional<uint32_t> cpuSlots; // Omitted keeps the mode's default CPU count.
		NetMatchMode mode = NetMatchMode::PvPSkirmish; // Shapes the roster: PvP (a team per peer), co-op PvE (one shared team vs CPU), PvPvE (teams + CPU).
		std::optional<bool> brainlessHumansSpectate; // Host rule: the round survives the last human brain. Unset takes the host's Gameplay setting.
		// The host's saved session options. Unset keeps the config default; NetMatchService::SeatSavedOptions fills them from the settings.
		std::optional<NetMatchDelayPolicy> delayPolicy;
		std::optional<uint16_t> slowPlayerBoundTicks;
		std::optional<NetSlowPlayerPolicy> slowPlayerPolicy;
		std::optional<uint8_t> idleWaitMinutes;
		std::optional<bool> automaticRepair;
		std::optional<uint16_t> pathHorizonTicks;
		std::optional<uint8_t> frameRedundancyTicks;
		// The host's checkpoint cadence in simulation seconds; 0 disables autosaves. Unset keeps the
		// run's AutosaveSeconds setting/override, so a request that names nothing changes nothing.
		std::optional<uint32_t> autosaveSeconds;
		bool resyncOnDesync = false; // A runtime desync reloads everyone from the host's snapshot instead of aborting the match.
		bool rejoin = false; // A seat coming back on its ticket: its fresh session walks the rejoin phases.
		bool dedicated = false; // Host only: keep lockstep peer hostPeerId but seat no human slot there.
		bool persistentWorld = false; // Host only: an indefinitely running world, never a last-brain or rematch.
		// World host only: open a new round from the scene instead of resuming the world's newest
		// checkpoint. The world keeps its UUID; its old checkpoints stay for retention to prune.
		bool worldFresh = false;
		std::string worldId; // Set after the host advances its durable identity; empty off a world.
		uint64_t worldBoot = 0;
		// Host-authored world capacity. Omitted fields take the Persistent World preset's defaults.
		std::optional<std::array<uint8_t, 4>> worldTeamCapacity;
		std::optional<uint8_t> worldMaxSpectators;
		std::optional<uint16_t> worldRespawnDelaySeconds;
		std::string sessionId; // Client only: join the directory session with this id instead of an address.
		// Filled by the service from the checkpoint's own restart manifest, never by a caller: the
		// configuration the peers agreed on, republished as the next revision.
		std::optional<NetMatchConfig> resumeConfig;
		// Host only: restart a match that died with its host. The lobby reopens on the checkpoint's own
		// manifest (the agreed roster, the seats and the admission), so the request's roster is ignored.
		std::string resumeMatchId;
		uint64_t resumeTick = 0; // 0 takes the newest resumable checkpoint of that match.

		/// Accepts a direct address or the session address shown by the Internet game list.
		void SetJoinAddress(const std::string& value) {
			const bool session = value.starts_with("session:");
			address = session ? std::string() : value;
			sessionId = session ? value.substr(8) : std::string();
		}
	};

	inline NetMatchServiceRequest TicketRejoinRequestFromRecord(const NetH4TicketRecord& record, const std::string& playerName) {
		NetMatchServiceRequest request;
		request.host = false;
		request.address = record.hostAddress;
		request.sessionId = record.directorySessionId;
		request.playerName = playerName.empty() ? "Client" : playerName;
		request.resyncOnDesync = true;
		return request;
	}

	inline std::string ResolveTicketJoinAddress(const NetH4TicketRecord& record, const std::string& requestSessionId, const std::string& requestAddress, const std::string& directoryResolvedAddress, bool iceDial) {
		const std::string sessionId = !record.directorySessionId.empty() ? record.directorySessionId : requestSessionId;
		if (!directoryResolvedAddress.empty()) {
			return directoryResolvedAddress;
		}
		if (!sessionId.empty() && iceDial) {
			return "session:" + sessionId;
		}
		if (!record.hostAddress.empty()) {
			return record.hostAddress;
		}
		if (!sessionId.empty()) {
			return "session:" + sessionId;
		}
		return requestAddress;
	}

	/// Polls a configured directory client until it answers a list or the budget runs out; the rows it
	/// returns are what a rejoin re-resolves against.
	std::vector<NetDirectorySessionRow> BrowseSessionRows(NetDirectoryClient& browse, uint64_t budgetMs, const std::function<bool()>& cancelled);

	/// Whether a browsed row is the host the rejoin prompt is waiting for: the same directory session,
	/// listed as a lobby or a running match. A world's session id is its own UUID, so a world answers
	/// this under every boot it ever takes.
	inline bool NetDirectoryRowIsWatchedHost(const NetDirectorySessionRow& row, const std::string& sessionId) {
		return !sessionId.empty() && row.sessionId == sessionId && (row.state == "running" || row.state == "lobby");
	}

	/// A stored ticket belongs to this join only when it names the host this request dials or the session
	/// it joins; a record left by another host is not a re-resolve of this one.
	inline bool TicketMatchesRequest(const NetH4TicketRecord& record, const std::string& requestSessionId, const std::string& requestAddress) {
		if (!record.directorySessionId.empty() && record.directorySessionId == requestSessionId) {
			return true;
		}
		return !record.hostAddress.empty() && record.hostAddress == requestAddress;
	}

	/// The address a ticket rejoin dials: the row the directory browse found for the stored session, else
	/// the ticket's own address or session id.
	inline std::string ResolveTicketJoinAddressFromRows(const NetH4TicketRecord& record, const std::string& requestSessionId, const std::string& requestAddress, const std::vector<NetDirectorySessionRow>& rows, const NetDirectoryLocalIdentity& local, bool iceDial) {
		const std::string sessionId = !record.directorySessionId.empty() ? record.directorySessionId : requestSessionId;
		std::string resolved;
		if (!sessionId.empty() && !rows.empty()) {
			NetIceJoinTarget target;
			if (NetIceResolveSessionRow(rows, local, sessionId, &target, nullptr, TicketMatchesRequest(record, requestSessionId, requestAddress)).empty() && !target.address.empty()) {
				resolved = target.address;
			}
		}
		return ResolveTicketJoinAddress(record, requestSessionId, requestAddress, resolved, iceDial);
	}
	/// The host's saved match defaults: the versioned template a new hosted lobby seeds its draft
	/// from. It holds only what a host chooses - never an occupant, a credential, a session epoch or
	/// a runtime peer id - so a template can be copied between machines without carrying identity.
	struct NetHostDefaultsTemplate {
		/// A seat's intent by position: the team a host wants on it, not who sits there.
		struct Seat {
			uint8_t team = 0;
			bool cpu = false;
			uint16_t delayFrames = 0;
		};
		uint16_t version = 2;
		NetMatchStandardRules rules; //!< Activity, site, mode, the original rule values and the team rules.
		uint8_t peerCount = 2;
		bool dedicated = false;
		NetMatchDelayPolicy delayPolicy = NetMatchDelayPolicy::Auto;
		uint16_t slowPlayerBoundTicks = NetMatchConfigUtil::c_DefaultSlowPlayerBoundTicks;
		NetSlowPlayerPolicy slowPlayerPolicy = NetSlowPlayerPolicy::Substitute;
		uint16_t inputDelayFrames = 0;
		bool autosaveEnabled = false;
		uint32_t autosaveIntervalSeconds = 0;
		uint8_t idleWaitMinutes = 10;
		bool automaticRepair = true;
		uint8_t frameRedundancyTicks = NetMatchConfigUtil::c_DefaultFrameRedundancyTicks;
		std::vector<Seat> seats;
	};

	/// Reads and writes the host-defaults template. The format is this class's own, versioned
	/// separately from the wire: an unknown key is ignored with a printed line, and a template a
	/// newer build wrote is refused with its version named instead of being half-read.
	class NetHostDefaults {
	public:
		static constexpr uint16_t c_Version = 2;

		/// The template a host's current draft would be saved as.
		static NetHostDefaultsTemplate FromConfig(const NetMatchConfig& config);
		/// Seeds a draft with the saved defaults. Refuses without touching the draft when the result
		/// would not be a valid configuration, so a stale template cannot produce an unlaunchable lobby.
		static bool ApplyTo(const NetHostDefaultsTemplate& saved, NetMatchConfig& config, std::string* error = nullptr);
		static std::string Serialize(const NetHostDefaultsTemplate& saved);
		static bool Parse(const std::string& text, NetHostDefaultsTemplate& out, std::string* error = nullptr);
		/// Reads the template beside Settings.ini. False with an empty error means there is none yet.
		static bool Load(NetHostDefaultsTemplate& out, std::string* error = nullptr);
		static bool Save(const NetHostDefaultsTemplate& saved, std::string* error = nullptr);
	};

	/// A presentation-only record of the finished round; never restored into the simulation.
	struct NetMatchSummary {
		struct Peer {
			uint8_t peerId = 0;
			std::string name;
			int team = -1;
			uint16_t seat = 0;
			uint16_t inputDelayFrames = 0;
			uint32_t holds = 0;
			uint32_t substitutions = 0;
			uint32_t rejoins = 0;
			uint64_t longestWaitMs = 0;
		};
		std::string result;
		int winnerTeam = -1;
		uint64_t runningTicks = 0;
		std::vector<Peer> peers;
		uint32_t resyncs = 0;
		uint32_t drops = 0;
		uint32_t reclaims = 0;
		uint32_t substitutions = 0;
		std::string paceJson = "{}";
		std::string identityLine;
		/// Formats the recorded duration at 60 ticks per second.
		std::string DurationText() const;
		/// Formats the single lobby line and the complete dialog body.
		std::string LineText() const;
		std::string DetailsText() const;
		/// Formats the identity, reading and caching the executable hash on first use.
		std::string IdentityText() const;
	};

	/// The in-place heal cap, held as a window of sim time instead of a process lifetime: a persistent
	/// world that heals once a day heals forever, while a match that cannot settle still stops after
	/// three heals inside the window. A sim time before the newest heal means the round was resumed from
	/// an older checkpoint, which starts the window again.
	class NetMatchHealWindow {
	public:
		static constexpr size_t c_Cap = 3;
		static constexpr long long c_WindowSeconds = 600;

		bool Allowed(long long nowSimTicks, long long ticksPerSecond) {
			Trim(nowSimTicks, ticksPerSecond);
			return m_Heals.size() < c_Cap;
		}
		void Note(long long nowSimTicks, long long ticksPerSecond) {
			Trim(nowSimTicks, ticksPerSecond);
			m_Heals.push_back(nowSimTicks);
			++m_Total;
		}
		size_t InWindow() const { return m_Heals.size(); }
		uint64_t Total() const { return m_Total; }

	private:
		void Trim(long long nowSimTicks, long long ticksPerSecond) {
			if (!m_Heals.empty() && nowSimTicks < m_Heals.back()) m_Heals.clear();
			const long long window = c_WindowSeconds * std::max<long long>(1, ticksPerSecond);
			while (!m_Heals.empty() && nowSimTicks - m_Heals.front() >= window) m_Heals.pop_front();
		}

		std::deque<long long> m_Heals; //!< Sim times of the heals still inside the window, oldest first.
		uint64_t m_Total = 0;
	};

	class NetMatchService : public Singleton<NetMatchService> {
	public:
		// Defined in the .cpp: the dispatcher member is only a declaration in this header.
		NetMatchService();
		~NetMatchService();

		/// Turns the H4 admission plane off for a run. It is on by default; this exists so a two-peer
		/// gate can be bisected against the pre-admission handshake without a rebuild.
		static void SetAdmissionEnabled(bool enabled) { s_AdmissionEnabled = enabled; }
		static bool IsAdmissionEnabled() { return s_AdmissionEnabled; }
		/// Overrides this run's checkpoint cadence in simulation seconds; zero disables it.
		static void SetAutosaveSeconds(uint32_t seconds) {
			s_AutosaveSeconds = seconds;
			s_AutosaveSecondsOverridden = true;
		}
		/// Applies the saved cadence while preserving any command-line override.
		static void SetAutosaveSecondsSetting(uint32_t seconds) {
			if (!s_AutosaveSecondsOverridden) s_AutosaveSeconds = seconds;
		}
		/// Gets this run's checkpoint cadence, including its command-line override.
		static uint32_t GetAutosaveSeconds() { return s_AutosaveSeconds; }
		static constexpr uint32_t c_MaxAutosaveIntervalSeconds = 3600; // An hour is the longest cadence a host may announce.
		static constexpr uint32_t c_MinAutosaveIntervalSeconds = 60; // A minute is the shortest; 0 stays off.
		/// The cadence a running match keeps: the command-line override when one was given, else the host's announced option.
		static uint32_t MatchAutosaveSeconds(const NetMatchConfig& config) {
			if (s_AutosaveSecondsOverridden) return s_AutosaveSeconds;
			return config.autosaveEnabled ? config.autosaveIntervalSeconds : 0;
		}
		/// Runs only after a complete lockstep tick, outside paused ticks and preview frames.
		/// A completed tick's committed frame joins the catch-up history a returner replays; a paused tick's too.
		void AppendCommittedJoinFrame(uint64_t tick);
		void AutosaveAtTickBoundary(uint64_t tick);
		/// One entry of the checkpoint schedule on the committed stream.
		struct CheckpointNote { uint8_t sender = 0; uint8_t kind = 0; uint64_t tick = 0; };
		/// What one completed tick hands the checkpoint schedule.
		struct AutosaveTickInput {
			uint64_t tick = 0;
			int64_t now = 0; //!< The tick's sim time.
			size_t unwritten = 0; //!< Captures this peer's writer has not finished.
			std::vector<CheckpointNote> applied; //!< The schedule entries the tick's committed frame carried.
			std::vector<uint64_t> finished; //!< Captures this peer's writer finished since the last boundary.
			std::set<uint8_t> writers; //!< Host: every peer that captures on the schedule.
			uint16_t lead = 0; //!< Host: ticks between naming a capture and taking it.
			bool activationPending = false; //!< Host: a seat's agreed activation is still ahead.
			bool startupPending = false; //!< Host: the round's agreed first frame is still ahead.
		};
		struct AutosaveTickOutput {
			bool capture = false; //!< This peer captures at this tick.
			std::vector<CheckpointNote> send; //!< Entries this peer puts on its committed stream.
		};
		/// The checkpoint schedule at one tick boundary, with the capture and the stream left to the caller.
		/// Every peer captures at each tick the host names; the host names the next one only once every
		/// writer has reported the last one finished, so no peer holds more than one unwritten capture.
		AutosaveTickOutput StepAutosaveSchedule(const AutosaveTickInput& input);
		/// Whether this host's capture at the tick is the one a joining member waits for.
		bool IsJoinCaptureTick(uint64_t tick) const { return m_IsHost && m_OpenCaptureForJoin && tick == m_OpenCaptureTick; }
		/// Whether the round named a capture for this tick: every peer collects every Lua state at its end, so the garbage each
		/// capture sees is the same on every peer and an image restored from it carries none the others still hold.
		bool IsNamedCaptureTick(uint64_t tick) const { return m_ScheduledCaptures.contains(tick); }
		/// Host: the peers that capture on the schedule at the tick, itself included.
		std::set<uint8_t> CheckpointWriters(uint64_t tick) const;
		/// Forgets the schedule a previous round named.
		void ResetCheckpointSchedule();
		/// Forgets the capture the host waits on across a heal, keeping the match's chain and the captures already named.
		void ForgetOpenCaptureOnHeal();
		/// Applies a finished capture's verdict to the world bookkeeping it stood for.
		void ApplyAutosaveVerdict(uint64_t tick, bool joinCapture, bool archived);
		/// Settles the awaited capture a writer verdict names; a verdict nobody awaits changes nothing.
		void ResolveAwaitedAutosave(uint64_t tick, bool archived);
		/// Takes every verdict the writer thread has finished since the last tick boundary; returns the captures they finished.
		std::vector<uint64_t> TakeAutosaveVerdicts();
		/// The one capture of this match: the tick's agreed lockstep state is stamped onto the identity
		/// here, so an interval checkpoint and a world's on-demand bootstrap capture carry the same
		/// owners and applied sequences and a restart resumes on them.
		bool SaveStampedAutosave(uint64_t tick);
		/// Cuts a recording world's segment at the checkpoint just captured, and opens the held segment
		/// as soon as that checkpoint's archive names its world-structure digest.
		void RollWorldReplaySegment(uint64_t tick);
		void SealWorldReplaySegment();

		/// Which recording a round of a persistent world opens: a segment that chains onto the
		/// checkpoint it stands on, or an ordinary file. A resume and a heal both stand on one.
		struct RoundRecordingPlan {
			bool segment = false;
			uint64_t tick = 0;
			std::string digest;
		};
		/// @param resumeTick The tick a resumed round stands on, 0 when this round did not resume.
		/// @param healTick The tick a healed round rewound to, 0 when this round is not a heal.
		static RoundRecordingPlan PlanRoundRecording(bool persistentWorld, bool worldIdentityValid,
		                                             uint64_t resumeTick, const std::string& resumeDigest,
		                                             uint64_t healTick, const std::string& healDigest);
		/// The lockstep state a match resumed from a checkpoint starts on, derived from the agreed
		/// configuration alone so every peer builds the same one whether it loads its own copy of the
		/// checkpoint or is streamed the host's. A restarted match has nothing in flight.
		static NetResyncState BuildResumeState(const NetMatchConfig& config, uint64_t savedTick, uint64_t sourceRound, const std::string& matchId, const AutosaveSideState& sideState);
		/// What a world segment needs before its records can play: the checkpoint it stands on, that
		/// checkpoint's manifest and the lockstep state the sim stands up with. A non-empty refusal says
		/// why the segment cannot play here and nothing else is filled.
		struct WorldSegmentPlayback {
			std::string refusal;
			AutosaveDescriptor checkpoint;
			AutosaveManifest manifest;
			NetResyncState resumeState;
			uint64_t startFrame = 0; //!< The checkpoint's tick + 1, where the records begin.
		};
		/// Checks a segment header against the checkpoints on this machine and builds that staging. The
		/// same check answers the first segment and every chain boundary.
		static WorldSegmentPlayback PrepareWorldSegmentPlayback(const std::filesystem::path& directory, const NetWorldSegmentHeader& header,
		                                                       const NetMatchConfig& config, uint64_t firstRecordedFrame);
		/// The hash a resume offer carries and a peer answers against, taken over the one rendering of
		/// the agreed side state so both sides compare the same bytes.
		static std::string HashSideState(const AutosaveSideState& sideState);
		/// Whether the checkpoint a peer holds is the one the host offered: the same world at that tick
		/// and the same agreed lockstep state to resume it on.
		static bool ResumeOfferMatches(const NetLobbyResume& offer, const std::string& worldDigest, const std::string& sideStateHash);
		/// Whether this peer holds the checkpoint a resumed host offers, and records it as the one to load.
		bool AnswerResumeOffer(const NetLobbyResume& offer);
		/// The id every peer of this match writes its checkpoints under.
		/// A copy under the lock: a resume and a join both assign this while the main loop reads it.
		std::string GetAutosaveMatchId() const {
			std::lock_guard<std::mutex> lock(m_Mutex);
			return m_AutosaveMatchId;
		}
		/// Records the checkpoint the host named for a heal: this peer pins it against retention and says
		/// whether it holds a restorable copy. The choice is never recomputed locally.
		/// @param known The descriptor of that same checkpoint when the caller already validated it, so the
		/// host does not read the archive a second time to answer a question it just answered.
		void NoteRewindAnchor(const std::string& matchId, uint64_t tick, bool host, const AutosaveDescriptor* known = nullptr);
		struct RewindAnchor {
			std::string matchId;
			uint64_t tick = 0;
			bool heldLocally = false;
		};
		RewindAnchor GetRewindAnchor() const;
		/// §11: the multiprocess reconnect test shares one Userdata, so each process gets its own
		/// recovery-record path instead of racing over the default one.
		static void SetTicketStorePath(std::string path);
		/// The host's ban list: a dedicated host keeps its own beside its configuration, and the
		/// multiprocess gates keep one per process instead of sharing the default.
		static void SetHostBanStorePath(std::string path);
		/// Phase B, client: ask the host for this seat instead of joining one. The UI (B2) sets it from
		/// the roster; the gate drivers set it from the command line.
		static void SetApplyForSeat(bool enabled, uint16_t stableSeat);
		/// Phase B, host: stand in for the moderator in an unattended gate - approve the first
		/// applicant for this seat after the delay, and optionally withdraw the approval again.
		static void SetAutoSubstitute(bool enabled, uint16_t stableSeat, uint64_t delayMs, bool thenCancel);
		/// A joiner finishes its startup and then waits for this file before it connects, so a gate can
		/// place a second holder of one ticket at a chosen moment of the match instead of at boot time.
		static void SetJoinWaitPath(std::string path);
		/// Polls `path` every 100 ms until it exists. False (with `error`) when `budgetMs` runs out.
		static bool WaitForJoinTrigger(const std::string& path, uint64_t budgetMs, std::string* error);
		static constexpr uint64_t c_JoinWaitBudgetMs = 120000;
		static constexpr uint64_t c_JoinWaitPollMs = 100;
		/// How long a finished match's rematch lobby waits for every peer to come back before the
		/// service destroys it and releases the session, the seats and the directory lease.
		static constexpr uint64_t c_CompletedLobbyExpiryMs = 600000;

		bool Start(const NetMatchServiceRequest& request, std::string* error = nullptr);
		bool CanSealA7Journal() const;

		/// Reconvenes a completed match's still-connected session in the lobby for a rematch.
		/// Fails (and settles the service into Failed) when the session was lost.
		bool ReturnToLobby(std::string* error = nullptr);

		/// Recovers a desynced match: the host snapshots its state and streams it through the lobby
		/// round; every peer relaunches from the identical file. Requires the session to be alive.
		bool ResyncMatch(std::string* error = nullptr);
		/// Read-only: a Repair Match press can run now - a live session, resync allowed, not over,
		/// and no restore already in flight. CanResyncLocked's conditions without its failure
		/// side-effects, so a per-frame UI poll never moves the service state.
		bool CanResyncMatch() const;
		// The shared stop lets the game loop own snapshot capture and relaunch on both peers.
		bool RequestHostRepair(std::string* error = nullptr);
		bool NeedsHostOptionsCorrection() const;
		/// Read-only repair progress for the Recovery page: inFlight while the heal is open, the
		/// snapshot bytes moved so far, and the open (or last finished) heal's elapsed ms.
		void GetResyncStatus(bool* inFlight, uint64_t* bytes, uint64_t* elapsedMs) const;
		/// The lobby's directory visibility: 0 LAN only (no held row), 1 hidden lease, 2 listed.
		int GetDirectoryVisibility() const;
		/// Moves the lobby's directory visibility through the held lease: 0 retracts the row (LAN
		/// only - re-listing needs a new hosted session), 1 keeps the lease hidden, 2 lists it.
		/// False when there is no lease to move. The bound ICE identity never changes.
		bool SetDirectoryVisibility(int visibility);
		void NoteResyncRelaunched();
		bool IsHostMigrationRepairPending() const;
		bool IsResyncOnDesyncEnabled() const { return m_ResyncOnDesync; }
		/// The snapshot file the next launch must load instead of a fresh activity ("" = none).
		std::string TakePendingResyncLoad();
		bool HasPendingResyncLoad() const;
		/// Stages the pending snapshot for launch: the world state is the file's, the player seats
		/// are per-peer, and the funds/roster ride the snapshot untouched.
		bool StageResyncedMatchLaunch(std::string* error = nullptr);
		void Destroy();
		void Update();

		/// Watcher: tell the world whether this player wants a seat when one frees. A declining
		/// watcher keeps its stream and promotion passes it over.
		bool SetWorldSpectatorDeclinesPromotion(bool declines);
		/// What this watcher last told the world; false means it wants the next free seat.
		bool WorldSpectatorDeclinesPromotion() const { return m_WorldSpectatorDeclinesPromotion; }
		void SetReady();
		void RequestStart();
		void ReportRuntimeError(const std::string& error);
		void Complete(const std::string& reason);
		void FinishMatch(const std::string& result);
		/// Ends the match locally as a clean leave: the other peers keep playing (N-peer) or hear
		/// "player left" (2-peer); the session objects stay alive exactly like FinishMatch. §7's leave
		/// exchange runs first, on the worker, so the ticket is answered while the link is still up.
		void LeaveMatch(const std::string& result);
		/// Blocks until the worker has finished. A report written before a leave settles would describe
		/// the exchange as unacknowledged when it was not.
		void WaitForPendingWork();

		bool ConsumeReadyToLaunch(std::string& outActivityPreset);
		void PreparePrivateRejoinCheckpoint();
		/// Host: the lockstep state a returning seat's base carries beside its archive, read at the base's own tick.
		bool ReadPrivateBaseLocked(uint64_t tick, NetWorldCheckpointImage& image, std::string* error);
		/// Host: a returning seat is waiting and the private base it would load is missing, abandoned or older than one capture interval.
		bool PrivateBaseWantedLocked(uint64_t nowMs) const;
		/// Whether a private base taken earlier is due again, for a seat held now or one returned after the base was taken.
		/// steadyCaptureMs is the median of the last captures past the round's first, or negative before there is one.
		static bool PrivateBaseRefreshDue(bool seatHeld, uint64_t staleFrom, uint64_t baseTick, double steadyCaptureMs);
		/// The median of the given capture costs, or -1 when there are none.
		static double SteadyCaptureMs(const std::deque<double>& costs);
		/// Whether the round's goodbye is owed to a ready seat at the round's end: one the round does not use, or one still under the AI at its last frame.
		static bool EndedRoundOwesGoodbye(bool coordinatorUsesPeer, bool seatUnderAIAtEnd);
		/// Refuses, with the round's goodbye, every ready peer of the session the ended round does not use, and with
		/// joiningToo every connection still in its handshake: a returning seat's, when a held seat can still be coming back.
		static void RefuseEndedPeers(NetSession& session, const NetLockstepCoordinator& coordinator, const std::string& reason, bool joiningToo);

		/// Runs the mid-match session upkeep: drains the reconnect-handshake events the coordinator
		/// handed over, and (host) turns a newly Ready session peer into a resync-for-rejoin.
		void PumpSessionEvents();
		/// Consumes the host's current seat snapshot on the game thread.
		void PumpSeatPresence();
		/// The reconnect UX state machine (§11): auto-retry, the stored-ticket offer and the roster's
		/// dropped/reclaiming marks. Game-thread only.
		NetReconnectUx& GetReconnectUx() { return m_ReconnectUx; }
		const NetReconnectUx& GetReconnectUx() const { return m_ReconnectUx; }
		/// Reads the cached moderation view; actions require a running match on the game thread.
		std::vector<NetH4ModerationSeat> GetModerationSeats() const;
		/// The seat-presence plane — where dropped seats get their reclaim-hold marks.
		const NetSeatPresence& GetSeatPresence() const { return m_SeatPresence; }
		NetH4ModerationResult ApplyModeration(const NetModerationSelection& selection, NetModerationAction action);
		/// Host: close this holder without a reclaim hold. The host confirmation dialog calls this.
		NetKickBanResult RemoveParticipant(const NetModerationSelection& selection, NetParticipantRemovalAction action);
		NetKickBanResult GetLastKickBanResult() const;
		NetParticipantRemovalIssue GetLastRemovalIssue() const;
		NetKickBanResult UnbanParticipant(const NetAuthBytes32& identity);
		std::vector<NetHostBanRecord> GetBanRecords() const;

		/// Remembers whether the last join target was a persistent world, so a ticket rejoin hellos 5/23.
		void NoteJoinTargetPersistentWorld(bool world) { m_LastJoinTargetPersistentWorld = world; }
		/// Re-enters the match this process was dropped from, using the stored recovery record.
		bool BeginTicketRejoin(std::string* error = nullptr);
		bool BeginHeldRejoin(std::string* error = nullptr);
		/// Held client: its rejoin found the host gone, so it rejoins the next peer the match's successor order names.
		/// @return Whether an attempt started; false when the failure was not the host's departure or no successor is left.
		bool BeginHeldRejoinOnNextHost(std::string* error = nullptr);
		/// How far every rejoin this host is serving has come: its admission, its phase, the image staged for
		/// it, the transfer it has acknowledged and the tail it has consumed. The goodbye drain watches this
		/// beside the round's own progress, because a rejoin commits no frame until it is back in the round.
		uint64_t RejoinProgressSum() const;
		/// Whether the host's goodbye has been heard, and the frame the round ended on (0 when it named none).
		bool HostGoodbyeSeen(uint64_t& finalFrame) const;
		/// The request a stored ticket rejoins with. The world flag is the ticket's own, so a relaunch
		/// against a world host still hellos on the world plane.
		static NetMatchServiceRequest BuildTicketRejoinRequest(const NetH4TicketRecord& record, const std::string& playerName, bool liveWorldTarget);
		static NetMatchServiceRequest BuildHeldRejoinRequest(const NetH4TicketRecord& record, const std::string& playerName, bool liveWorldTarget, const NetMatchServiceRequest& liveRoute);

		/// The joiner's catch-up step over one lobby pump: applies the tail that arrived, adopts the
		/// announced E and reports what the sim has applied. The value it sends is the report the host
		/// schedules activation from.
		/// One joiner step: the arrived tail, its E and the report it sends back.
		/// @param outRefusal The world's refusal code when the host turned this joiner away; 0 otherwise.
		static void StepWorldJoinCatchUpClient(NetLobbySession& lobby, NetWorldCatchUpClient& catchUp, uint64_t* outRefusal = nullptr);
		/// Sends one bounded run of committed tail frames to a bootstrap and stamps what left.
		static void SendWorldJoinTailTo(NetLobbySession& lobby, NetWorldJoinHost& host, const NetWorldJoinSession& session);
		/// Answers one refused connection on the world's reserved refusal id. Binding re-points a
		/// known remote, so this must never take a watcher's or a member's id.
		static bool AnswerWorldJoinRefusal(NetLobbySession& lobby, NetPeerId connection, NetWorldJoinRefusal refusal);
		/// The bootstrap a lobby report belongs to: a bootstrap's own lobby id first, then a ready peer.
		static NetPeerId ResolveWorldReportConnection(const NetWorldJoinHost& host, const std::vector<NetSessionPeerInfo>& readyPeers, uint8_t fromPeer);
		/// Applies one world-join report to the host's plane and sends the E it earns.
		/// @return The activation tick this report earned, 0 when it announced none. The caller hands
		/// it to the round: every sender spells its observation keys out again from there.
		static uint64_t ApplyWorldJoinReport(NetLobbySession& lobby, NetWorldJoinHost& host, const NetLobbySession::WorldJoinReport& report, NetPeerId connection, uint64_t nowFrame, uint64_t nowMs);
		/// Whether a bootstrap can be started at all. A bootstrap with no world lobby id never can, so
		/// the world ends it instead of building its image again every tick.
		static bool WorldBootstrapCanStart(const NetWorldJoinSession& session, std::string* reason);
		/// The world slot a clean leave frees, named the way the Release transition names it.
		struct WorldCleanLeave {
			uint8_t peerId = 0;         //!< The slot's lockstep id, which is what the member played on.
			uint16_t stableSeat = 0;    //!< The admission seat the slot was bound to.
			uint32_t holderGeneration = 0;
			int8_t team = -1;
			NetPeerId connection = c_InvalidNetPeerId; //!< The departed member's bootstrap.
		};
		/// The slot whose holder left through the admission plane: its seat is open again while the
		/// slot is still held by an Active member. Keyed on the SEAT the slot is bound to, never on
		/// the seat's lockstep id - a promoted watcher plays on a slot its own seat does not name, so
		/// a leftover row naming that id would release whoever holds it next.
		static bool FindWorldCleanLeave(const std::vector<NetH4SeatStatus>& statuses, const NetWorldJoinHost& world, WorldCleanLeave& outLeave);
		/// A slot whose seat the AI holds while its player closed that seat by leaving: the member chose to go, so the seat is
		/// released (the AI keeps its units) and the slot opens for a new join. A seat still committed, dropped or mid-reclaim
		/// is still its member's. The departed returner's bootstrap, if any is left, is named in the result.
		static bool FindHeldWorldCleanLeave(const std::vector<NetH4SeatStatus>& statuses, const NetWorldJoinHost& world, const std::set<uint8_t>& aiHeldPeers,
		                                    WorldCleanLeave& outLeave);
		/// The world slots a dropped or reclaiming seat is waiting for, by the slot each seat holds.
		/// A promoted watcher plays on a slot its seat's lockstep id does not name, so a hold keyed on
		/// that id would leave its slot open and fence a slot nobody is coming back to.
		/// A held slot whose seat the AI plays waits for its own player the same way.
		static std::vector<uint8_t> WorldReclaimHoldSlots(const std::vector<NetH4SeatStatus>& statuses, const NetWorldMembership& membership,
		                                                  const std::set<uint8_t>& aiHeldPeers = {});
		/// The sim id and team the holder of an admission seat plays on. The world plane owns that
		/// answer: a member plays the slot its seat is bound to, whatever id its seat table names.
		/// An unbound seat is not the world's, so the caller keeps the seat's own pair.
		static NetH4SeatSimIdentity WorldSimIdentityOfSeat(const NetWorldMembership& membership, uint16_t stableSeat);
		/// Records what the lobby did with a bootstrap's image. A refusal is not a start: the bootstrap
		/// stays unstarted so the next pump retries it.
		static bool NoteImageTransferOutcome(NetLobbyStateTransfer outcome, NetLobbySession& lobby, NetWorldJoinHost& host, NetPeerId connection, uint64_t deliveredThrough);
		/// Ends the joiner's catch-up the moment its own coordinator runs: the round owns the wire and
		/// the pacing from there. Returns whether this call released it.
		static bool ReleaseWorldCatchUpOnceRunning(bool coordinatorRunning, NetWorldCatchUpClient& catchUp);
		static bool ReadCommittedJoinFrame(const NetLockstepCoordinator& coordinator, uint64_t tick, NetLockstepReadyFrame& ready);
		/// The image one finished archive describes. An entry the writer has not filled yields an
		/// image that is not valid, so nothing is published for it.
		static NetWorldCheckpointImage WorldImageFromAutosave(const ActivityMan::CompletedAutosave& entry, const NetWorldIdentity& identity,
		                                                     const NetMatchConfig& matchConfig, uint64_t membershipRevision, double captureMs);
		/// §11: reads the recovery record so the landing screen can offer a rejoin after a relaunch, or
		/// say exactly why it cannot. Read-only and safe to call repeatedly.
		void ScanStoredTicket();
		/// Whether the §11 retry schedule still has work, so the menu loop pumps the service whatever
		/// screen is up rather than only while the multiplayer screen is open.
		bool NeedsRecoveryPump() const;
		/// Whether a finished match still wants the menu loop's pump for its rematch lobby and kept
		/// directory lease. Not a recovery: the screens route a drop, not an ordinary match end.
		bool NeedsCompletedLobbyPump() const;
		/// Whether the host refused the last join because its match is already running, which is the
		/// only case §9b's applicant path exists for.
		bool WasJoinRefusedByALiveMatch() const;
		/// Asks the host for a seat instead of joining one: the same connection the join used, with
		/// §9b's application in place of the new-join request. The host picks the seat.
		bool BeginSubstituteApplication(const NetMatchServiceRequest& request, std::string* error = nullptr);

		NetMatchServiceState GetState() const;
		NetHostHandoverState GetHostHandoverState() const;
		bool IsHost() const { std::lock_guard<std::mutex> lock(m_Mutex); return m_IsHost; }
		bool WasEverStarted() const { return m_EverStarted.load(); }
		/// The host's router port-mapping state, for the lobby's status line. Game-thread only.
		struct PortMapStatus {
			bool enabled = false;    //!< This match's host asked the router for a mapping.
			bool done = false;       //!< The request settled: mapped, or the chain gave up.
			bool mapped = false;     //!< A mapping is held right now.
			std::string method;      //!< "natpmp"|"pcp"|"upnp" while mapped.
			std::string externalIp;
			uint16_t externalPort = 0;
			std::string error;       //!< Why the chain gave up; empty while running or mapped.
		};
		PortMapStatus GetPortMapStatus() const;
		NetLobbySnapshot GetLobbySnapshot() const;
		/// The match config the live lobby round adopted and every peer acknowledged - the host's
		/// published roster/rules/policy on each peer alike. Falls back to the request-derived config
		/// before the lobby's first publish (a joiner's placeholder reads the same way).
		NetMatchConfig GetLobbyMatchConfig() const;
		/// Host options transaction (§3.1): validates a complete draft against the adopted revision
		/// and stages it as the next-match intent. Rejects non-host calls, stale revisions, and any
		/// draft that reallocates a seated peer's slot. Returns true when the draft was accepted.
		bool SubmitHostOptions(uint64_t expectedRevision, const NetMatchConfig& draft, std::string* error = nullptr);
		/// The staged next-match draft, when Apply accepted one. The GUI re-seeds its next host
		/// options draft from it; a later Start publishes it like any new-lobby request.
		std::optional<NetMatchConfig> GetPendingHostOptions() const;
		/// Returns a copy that survives returning to the lobby and expires at the next match start.
		std::optional<NetMatchSummary> GetLastMatchSummary() const;
		/// Local chat send, presentation only. Reaches the session whether the lobby is still running
		/// on the worker or the match has handed it back; false when no session link exists.
		bool SendChat(uint8_t scope, const std::string& text);
		/// Drains the session's chat queue for the UI. Newest 64 are kept on the session side.
		std::vector<NetChatEntry> TakeChatEntries();
		std::vector<NetChatEntry> ChatHistory() const;
		/// "Input delay: N (auto, Rms ping)" / "(fixed)", from the announced match config. "" pre-lobby.
		std::string GetInputDelayText() const;
		/// The live host RTT on a client, or the largest connected peer RTT on the host.
		std::optional<uint32_t> GetMatchPingMs() const;
		/// Whether the current match is being restored from the host snapshot.
		bool IsMatchResyncing() const;
		/// The current seat holder's display name for presentation events.
		std::string GetPeerDisplayName(uint8_t peerId) const;
		std::string GetStatusText() const;
		std::string GetErrorText() const;
		/// The active session's route, separate from the saved preference for the next one.
		std::string GetConnectedRoute(uint8_t peerId = 0) const;
		std::string GetIceRoute() const {
			std::lock_guard<std::mutex> lock(m_Mutex);
			return m_IceRoute;
		}
		std::string GetNatModeText() const;
		std::string GetRelayError() const;
		/// How often the snapshot-load keepalive has taken the service lock and ticked the session.
		uint64_t GetSnapshotLoadKeepaliveTicks() const { return m_SnapshotLoadKeepaliveTicks.load(); }
		/// Proves the keepalive keeps ticking through a load that runs off the service lock.
		bool RunSnapshotLoadKeepaliveSelfTest(std::string* error);
		std::string BuildReportJson() const;
		/// Builds the match roster from the request. An empty scene keeps MakeDefault unless the caller
		/// already resolved one; a named scene overwrites the default after any launch-config rules.
		static bool BuildMatchConfig(const NetMatchServiceRequest& request, uint64_t sessionId, NetMatchConfig& outConfig, std::string* error = nullptr);
		/// The module a module-less activity preset belongs to, from the modules that define it. Reads no
		/// manager: the caller lists the candidates.
		static bool ResolveActivityModule(const std::string& preset, const std::vector<std::string>& definingModules, std::string& outModule, std::string* error = nullptr);
		/// Fills an unset request module with the loaded module that defines the preset.
		static bool SeatActivityModule(NetMatchServiceRequest& request, std::string* error = nullptr);
		/// Every loaded non-test GameActivity, including those with no compatible scene.
		static std::vector<NetHostActivityChoice> ListLoadedGameActivities();
		/// The scenario-menu walk: every non-test GameActivity with at least one compatible loaded scene.
		static std::vector<NetHostActivityChoice> ListHostActivities();
		/// Compatible scenes for one activity, in the same scene order the scenario menu uses.
		static std::vector<NetHostSceneChoice> ListHostScenes(const std::string& preset, const std::string& module);
		/// First compatible scene, Grasslands when the activity allows it.
		static bool ResolveHostScene(const std::string& preset, const std::string& module, std::string& sceneName, std::string& sceneModule);
		/// Fills an empty host scene from the loaded modules the way SeatActivityModule fills a module.
		static bool SeatHostScene(NetMatchServiceRequest& request, std::string* error = nullptr);
		/// Empty picker: the request still names P4 Alpha Duel and Base.rte.
		static void ApplyHostActivityFallback(NetMatchServiceRequest& request);
		/// Fills the request's unset options from the saved settings, where a real host starts a match.
		static void SeatSavedOptions(NetMatchServiceRequest& request);
		/// Builds diagnostic identity on request; match startup supplies the cached join inputs.
		bool RefreshDiagnosticIdentity(std::string* error = nullptr, double* buildMs = nullptr);
		/// Reads the identity's live manager inputs and keeps them for a build off this thread. Cheap:
		/// the module hashing that costs the second is left to the build below.
		bool CaptureDiagnosticIdentityInputs(std::string* error = nullptr);
		/// Hashes the captured inputs and caches the identity. Reads no manager, so the diagnostics
		/// worker runs it while the game thread keeps drawing.
		bool BuildCapturedDiagnosticIdentity(std::string* error = nullptr, double* buildMs = nullptr);
		/// Forgets captured inputs no bundle will build, so a refused request strands nothing.
		void DropCapturedDiagnosticIdentityInputs();
		/// Returns the cached join inputs without reading settings, modules, or simulation state.
		std::string ExportDiagnosticIdentity() const;
		/// Returns the last runtime error and heal record without exposing reconnect credentials.
		std::string ExportDiagnosticDesyncHeal() const;
		uint8_t GetLocalPeerId() const;
		int GetLocalTeam() const;

		static bool ResyncSnapshotAllowed(const Activity* activity);
		static NetRejoinAnswer ClassifyRejoin(const Activity* activity);
		void AnswerMatchOverRejoin(const std::string& result);
		/// The goodbye a rejoining seat reads: the round is over, and the frame it ended on.
		static std::string MatchOverGoodbyeText(uint64_t finalFrame);

		static const char* StateName(NetMatchServiceState state);

	private:
		std::string LiveInputDelayTextLocked() const;
		/// A match's transports, moved as one into a rematch or resync worker and back.
		struct TransportLink {
			// Defined in the .cpp, where the dispatcher type is complete.
			TransportLink();
			TransportLink(TransportLink&&) noexcept;
			TransportLink& operator=(TransportLink&&) noexcept;
			~TransportLink();

			std::unique_ptr<GnsTransport> ip;
			std::unique_ptr<NetMuxTransport> mux;
			std::unique_ptr<INetTransport> migrated;
			std::vector<NetTransportEvent> lobbyEvents;
#ifdef CCCP_WITH_GNS
			std::unique_ptr<GnsDirectorySignalDispatcher> dispatcher; //!< After the mux, so it is destroyed first.
#endif
			/// The session's wire: the mux if there is one, else the IP transport.
			INetTransport* Wire() const { return migrated ? migrated.get() : mux ? static_cast<INetTransport*>(mux.get())
				                                                                 : ip.get(); }
		};

		void WorkerMain(NetMatchServiceRequest request, NetIdentityManifest manifest, NetIdentityBuildOptions identityOptions);
		void DriveWorldJoins(uint64_t nowMs);
		void DrivePrivateMatchRejoins(uint64_t nowMs);
		/// Moves a returning seat's activation to the first frame the agreed park cannot reach and tells the returner.
		/// @return Whether the returner was told a new frame; false when it already used its re-announce.
		bool MovePrivateActivationPastPark(const NetWorldJoinSession& session);
		/// Opens the segment the round's last checkpoint names once its archive is written, so the ticks after it are recorded.
		void SealPendingWorldSegmentAtEnd();
		/// Host: the round has ended for a relaunch, or a relaunch is loading; moderation waits for the round it opens.
		bool RelaunchInFlightLocked() const;
		/// Host: ends the rejoin of every returner that has replayed past the bound without showing the headroom its activation needs.
		/// Answers the returners that cannot progress: one replaying below the round's rate is told once that it keeps catching
		/// up (its activation waits for headroom); one whose base cannot come - a decided match, or no image within two capture
		/// waits - is refused with the reason.
		void AnswerStalledReturnersLocked(uint64_t nowMs);
		bool PrivateReturnerInFlightLocked() const;
	public:
		enum class LoneElection { EndMatch, RejoinHost, HostForHeldSeats };
		/// What a survivor that finds no other live member does: an announced leave ends its match, a lost host with a held
		/// seat in the round is replaced by this peer so the held seats rejoin it, and a lost host with none is rejoined.
		static LoneElection LoneElectionOutcome(bool hostAnnounced, bool heldSeats);
	private:
		std::set<NetPeerId> m_SlowReturnersNoted; //!< Returners already told they keep catching up below the round's rate.
		/// Host: ends one returner's rejoin and tells its client why, so it tries again instead of waiting.
		void RefuseReturnerLocked(NetPeerId connection, const std::string& reason, const std::string& text);
		/// Client: points the ticket at a successor and rejoins it.
		bool RejoinSuccessorRoute(const NetMatchServiceRequest& route, std::string* error);
		/// Host: seals the successor capsule for a returning peer and sends it after the config, as a lobby round does.
		void SendSuccessorCapsuleToLocked(uint8_t member);
		/// Bounds a returning seat's wait on the private capture's writer: one fresh capture, then the seat stays with the AI.
		void BoundPrivateImageWait(uint64_t nowMs);
		void DriveWorldJoinClient(uint64_t nowMs);
		/// Held client: a round stopped by this peer's own seat hold, with its sim short of the hold frame, keeps its world and its
		/// connection and replays the committed tail from its own tick. Returns whether it began; otherwise the stop reloads an image.
		bool BeginInPlaceCatchUp();
		/// Client: names the world's own UUID in the stored ticket, so the return watch browses for the
		/// row the world re-registers under on its next boot.
		void AdoptWorldTicketSession(const NetMatchConfig& config);
		/// Host: watches each seated member's brain and authors one respawn per death.
		void DriveWorldSeatRespawns(uint64_t nowFrame);
		/// Publishes the newest archive the autosave writer has FINISHED, when it is newer than the
		/// image a bootstrap is already being served. Nothing here reads a file.
		void PublishFinishedWorldJoinImage();
		/// Writes a newly issued directory row token into the world identity record, so a reboot
		/// resumes the same row instead of leaving a stale one to expire.
		void PersistWorldDirectoryToken();
		/// Ships the published image to one bootstrap. `outUnstartable` reports a bootstrap that can
		/// never start, so the caller ends it instead of retrying it every tick.
		bool StartJoinerImageTransfer(const NetWorldJoinSession& session, std::string* error, bool* outUnstartable = nullptr);
		void PumpWorldJoinLobby(uint64_t nowMs);
		/// Host: a held seat's player reporting the tick its own state stands at gets the committed tail from there on its live connection.
		void OpenInPlaceRejoinLocked(const NetLobbySession::WorldJoinReport& report, const std::vector<NetSessionPeerInfo>& readyPeers, uint64_t nowMs);
		/// Held client: whether the match names a successor this seat could rejoin when its host is gone.
		bool HeldSeatHasSuccessorLocked() const;
		/// Held client: its host is gone and nobody else is left, so it plays the round on from its own committed state with the
		/// AI in every other seat and its own hold ended. Returns whether the round runs on it.
		bool HostAloneFromOwnStateLocked();
		bool PrepareReceivedWorldJoin(const std::vector<uint8_t>& bytes, const NetMatchConfig& adopted, std::string& pendingLoad, std::string* error);
		/// Restarts the silence windows of a session handed to a worker thread.
		void NoteSessionHandedToWorker(NetSession& session);
		/// Declares the private catch-up's park on the session the arming thread owns.
		void NoteWorldCatchUpArmed(NetSession& session);
		void WorkerRematchMain(TransportLink link, NetSession* sessionRaw, NetLockstepCoordinator* coordinatorRaw, NetMatchRunner* runnerRaw, bool departedHost);
		void WorkerResyncMain(TransportLink link, NetSession* sessionRaw, NetLockstepCoordinator* coordinatorRaw, NetMatchRunner* runnerRaw, std::vector<uint8_t> stateBytes);
		void StartSnapshotLoadKeepalive();
		void StopSnapshotLoadKeepalive();
		/// The live wire, by the same rule. Caller holds the lock.
		std::string GetConnectedRouteLocked(uint8_t peerId) const;
		INetTransport* ActiveWireLocked() const { return m_MigratedTransport ? m_MigratedTransport.get() : m_Mux ? static_cast<INetTransport*>(m_Mux.get())
			                                                                                                     : m_Transport.get(); }
		bool SealMigrationCapsule(uint8_t peerId, const NetHash32& configHash, std::vector<uint8_t>& sealed);
		bool SealMigrationCapsuleLocked(uint8_t peerId, const NetHash32& configHash, std::vector<uint8_t>& sealed);
		bool OpenMigrationCapsule(const NetLobbyMigration& capsule);
		bool OpenMigrationCapsuleLocked(const NetLobbyMigration& capsule);
		void PumpHostMigration();
		void PublishMigrationCapsulesLocked();
		std::unique_ptr<INetTransport> m_MigratedTransport;
		std::unique_ptr<NetLockstepCoordinator> m_MigrationFallbackCoordinator;
		std::atomic<bool> m_MigrationFallbackReady{false};
		uint64_t m_MigrationGeneration = 0;
		NetHash32 m_MigrationKey{};
		std::vector<uint8_t> m_MigrationAdmissionState;
		std::vector<uint8_t> m_LastMigrationAdmissionState;
		uint8_t m_MigrationAuthority = 0;
		std::vector<uint8_t> m_MigrationMembers;
		std::string m_MigrationDirectorySession;
		std::string m_MigrationDirectoryToken;
		uint64_t m_MigrationStatusUntilMs = 0;
		bool m_MigrationDirectoryResumePending = false;
		bool m_MigrationRepairPending = false;
		/// Hands the transports and dispatcher to a worker, caching the dispatcher's report. Caller holds the lock.
		TransportLink TakeTransportLinkLocked();
		/// Takes them back from a worker. Caller holds the lock.
		void RestoreTransportLinkLocked(TransportLink link);
		/// Refuses a resync with no live match or a lost session; a lost session fails the service. Caller holds the lock.
		bool CanResyncLocked(std::string* error);
		bool PrepareReceivedResync(const std::vector<uint8_t>& bytes, const NetLockstepCoordinator& coordinator, std::string& pendingLoad, NetResyncState& state, std::string* error, size_t* archiveBytes = nullptr);
		/// Host: publishes the match's admission export and the directory row it resumes beside its
		/// checkpoints, sealed for this install alone. Runs on the host pump, never on the sim thread,
		/// and only when the admission state, the directory row or a new checkpoint made it stale.
		void PublishRestartAdmission();
		/// Removes the admission file of a match this process is no longer checkpointing once no
		/// checkpoint of it is left. Game thread, once per ended round.
		void SweepRestartAdmission();
		/// A world host that stops cleanly leaves the committed tick it stopped on, with its manifest
		/// and admission file, so a restart loses nothing. Synchronous: the sim has stopped by here and
		/// the writer is waited on before the process leaves the world. Once per teardown.
		void WriteFinalWorldCheckpoint();
		/// Host: the resume the request asked for - the manifest's config, the sealed admission and the
		/// checkpoint to open on. Fills the request's roster and arms the resume, or says why it cannot.
		/// @param directory Which checkpoint store to read; empty means this install's own. A caller
		/// that names one reads exactly that directory, so a row never has to write into the player's.
		bool PrepareResume(NetMatchServiceRequest& request, std::string* error, const std::filesystem::path& directory = {});
		/// World host: points the request's resume at the world's own checkpoint chain, so a boot of an
		/// existing world reopens it through the one resume path instead of a second implementation.
		/// Runs before PrepareResume and reads the identity record without advancing it.
		bool ResolveWorldResume(NetMatchServiceRequest& request, std::string* error);
		/// The configuration a resumed world plays on: the checkpoint's own roster under THIS boot's
		/// identity, so no credential, envelope or key of the previous boot is live under it.
		static void SeatResumedWorldConfig(NetMatchConfig& config, const NetWorldIdentity& identity);
		/// Whether a teardown still owes a final world checkpoint, and at which committed tick. Pure,
		/// so the decision lives in one place and never moves with the caller's state.
		static bool FinalCheckpointTick(bool host, bool worldConfigured, bool alreadyWritten, uint32_t autosaveSeconds,
		                                bool lockstepActive, bool activityRunning, uint64_t nextFrame, uint64_t& outTick);
		/// The one purpose label the restart admission key is derived under.
		static constexpr const char* c_RestartAdmissionKeyLabel = "cccp-restart-admission-v1";
		/// 7e: polls the directory for the row the stored ticket names, so the rejoin prompt enables
		/// itself the moment that host comes back. Game thread, like the directory client it drives.
		void PumpHostReturnWatch(uint64_t nowMs);
		/// Stamps the identity every checkpoint carries with the configuration a restart reopens on.
		/// Caller holds the lock.
		void SeatRestartConfigLocked(const NetMatchConfig& config);
		/// This install's key for its own restart admission file; false without an identity key.
		bool DeriveRestartKey(std::array<uint8_t, 32>& key);
		/// The session the round is hosted on; the adopted match config carries the seats it offers.
		NetSessionConfig BuildSessionConfig(const NetIdentityManifest& manifest, const NetMatchServiceRequest& request, const NetMatchConfig& matchConfig) const;
		/// Applies the service's lobby start policy and request controls.
		void ConfigureLobbyStart(NetMatchRunnerConfig& config);
		void SetState(NetMatchServiceState state, std::string status, std::string error = "");
		/// Match end or the host leaving takes the directory row down now rather than at Destroy.
		/// Game-thread only, like the client it drives.
		void RetractDirectoryListing();
		/// Keeps only the registered row bound to this host's ICE identity. Game-thread only.
		bool ShouldKeepIceDirectoryLease() const;
		/// Hides the bound row while retaining its lease. Game-thread only.
		void HideDirectoryListing();
		/// Relists an acknowledged hidden lease or retracts a lost one. Game-thread only.
		void SettleKeptDirectoryLease();
		/// Host: waits for the register reply so the GNS identity can be pinned to the session id
		/// before any listen socket of this process opens. Worker thread; reads the published snapshot.
		bool WaitForDirectorySession(uint64_t budgetMs, std::string& sessionId, std::string& token) const;
		/// Host: registers first, pins the GNS identity to the session id, then opens both listens.
		/// Client: resolves the session id to a row and arms the join. Worker thread.
		bool SetUpIceTransport(const NetMatchServiceRequest& request, const NetIdentityManifest& manifest, NetMuxTransport& mux, NetSessionConfig& sessionConfig, std::string& joinAddress, NetIceJoinTarget& target, std::string* error);
		/// Builds the candidate policy from the saved settings and run overrides.
		static GnsP2PConfig BuildIceConfig(const SettingsMan& settings, const std::string& localIdentity, int localVirtualPort, const NetRelayConfig& relay = {});
		void UpdateRelayOffer(uint64_t nowMs);
		void PublishRelayOfferLocked(NetSession& session, INetTransport& wire);
		bool ReadRelayOffer(NetRelayConfig& offer) const;
		void SetRelayOfferLocked(const NetRelayConfig& offer);
		/// Retries a failed ICE connection once through the row's direct address.
		bool StartLobbyConnection(std::unique_ptr<NetMuxTransport>& mux, INetTransport& ip, NetSession& session, NetLockstepCoordinator& coordinator,
		                          NetMatchRunner& runner, NetMatchRunnerConfig& config, const NetIceJoinTarget& target,
		                          bool transportReady, bool& noDirectRoute, std::string* error);
		/// Keeps admission refusals distinct from a failed direct connection.
		static std::string SetupFailureStatus(const NetSession* session, bool noDirectRoute, bool relayFailed = false);
		/// The ICE virtual port a host listens on and a joiner dials.
		static constexpr int c_IceVirtualPort = 41011;
		static constexpr uint64_t c_IceRegisterBudgetMs = 30000;
		static constexpr uint64_t c_IceResolveBudgetMs = 30000;
		static constexpr uint32_t c_IceConnectBudgetMs = 15000;
		void JoinWorkerIfDone();
		/// Attaches the H4 admission plane to a freshly built session. Host: only with a live auth
		/// epoch, so a build without crypto keeps the pre-admission handshake and issues no tickets.
		/// False when a ban list that is present cannot be read; the host does not start on an
		/// admission plane that would admit the identities it names.
		bool AttachAdmissionPlane(NetSession& session, const NetMatchServiceRequest& request, const NetMatchConfig& matchConfig, const NetSessionConfig& sessionConfig, const NetIdentityManifest& manifest, std::string* error);
		/// The drop-frame ownership census. Called by the reconnect host, and only ever from inside
		/// PumpSessionEvents on the game thread - g_MovableMan is not safe to walk from anywhere else.
		static std::vector<NetH4LedgerActor> CollectDropOwnership(void* context);
		/// The world plane's answer for the admission plane, off the one seat-to-slot binding. Called
		/// by the reconnect host from inside this service's own calls, so it takes no lock of its own.
		static NetH4SeatSimIdentity SeatSimIdentitySource(void* context, uint16_t stableSeat);
		/// The H4 seat state the round consults before it adjudicates a lost transport. Called by the
		/// coordinator on the game thread, which never holds this lock.
		static NetLockstepSeatState QuerySeatState(void* context, uint8_t lockstepPeerId, NetPeerId transportPeerId);
		friend bool TestHoldResolutionPumpDoesNotRelock(std::string* error);
		friend bool TestAParkReachesTheSessionAWorkerOwns(std::string* error);
		friend bool TestARejoinWalksItsPhasesAndTheGoodbyeEndsItsTailReplay(std::string* error);
		friend struct HostOptionsLobbyRow;
		bool HostOptionsNeedCorrectionLocked() const;
		friend bool TestMatchOverRejoinFromWaitKeepsCoordinator(std::string* error);
		friend bool TestResumePreparesTheAgreedLobby(std::string* error);
		friend bool TestRosterTransitionsRecordHoldThenPresent(std::string* error);
		friend bool TestRosterBannerNamesThePlayerOnce(std::string* error);
		friend bool TestAiOnlyHostSeatsNoJoiner(std::string* error);
		friend bool TestPendingSessionEventSurvivesTeardown(std::string* error);
		friend bool TestServiceKick(std::string* error);
		friend bool TestServiceKickRejoin(std::string* error);
		friend bool TestStartingKickMarshals(std::string* error);
		friend bool TestUnreadableBanListHoldsAdmission(std::string* error);
		friend bool TestLobbyModerationRows(std::string* error);
		friend bool TestServiceReturnToLobbyFormsTheNextRoster(std::string* error);
		friend bool TestRematchAfterHostDeparture(std::string* error);
		friend bool TestResyncFailureAfterHostDeparture(std::string* error);
		friend bool ServiceRematchRoster(NetMatchService& service, const NetMatchConfig& played, uint8_t localSessionPeerId, NetMatchConfig& roster, std::string* error);
		friend bool TestFinishMatchDrainsFencedDisconnect(std::string* error);
		friend bool TestGnsStopCancelContracts(std::string* error);
		friend bool TestEndedWorldLateAdmission(std::string* error);
		friend bool TestServiceDirectoryIceLeaseKeepsIdentity(std::string* error);
		friend bool TestIceDefaultsAndOverrides(std::string* error);
		friend bool TestRelayOfferAndPolicy(std::string* error);
		friend bool TestRelayOfferRefresh(std::string* error);
		friend bool TestIceConnectionFallback(std::string* error);
		friend bool TestHandoverSnapshotStatus(std::string* error);
		friend bool TestDiscoveryOccupancy(std::string* error);
		friend bool TestServiceIceRematchPlaysTwoRounds(std::string* error);
		friend bool TestEndMatchWithHeldSeatKeepsItsLease(std::string* error);
		friend bool TestCompletedLobbyIsNotARecovery(std::string* error);
		friend bool TestCompletedLobbyExpires(std::string* error);
		friend bool TestCapturedWorldIdentityKeepsTheWorldStamp(std::string* error);
		friend bool TestServiceWorldJoinAdoptsConfig(std::string* error);
		friend bool TestChatSendRefusedOutsideCarry(std::string* error);
		friend bool TestServiceReportCarriesActivityPreset(std::string* error);
		friend bool RowRestartKey(NetMatchService& service, const std::filesystem::path& scratch, std::array<uint8_t, 32>& key, std::string* error);
		friend bool TestWorldRestartOpensOnCheckpoint(std::string* error);
		friend bool TestWorldFreshFlagOpensNewRound(std::string* error);
		friend bool TestWorldCleanStopWritesFinalCheckpoint(std::string* error);
		friend bool TestWorldBootstrapWaitsForLobby(std::string* error);
		friend bool TestWorldCaptureFollowsTheDeferredVerdict(std::string* error);
		friend bool TestWorldCaptureKeepsOneImageInFlight(std::string* error);
		friend bool TestNoCaptureIsNamedOverAPendingActivation(std::string* error);
		friend bool TestNoCaptureIsNamedBeforeTheAgreedFirstFrame(std::string* error);
		friend bool TestPeersCheckpointTheSameTicks(std::string* error);
		friend bool TestACaptureNamedIntoAParkOpensTheNext(std::string* error);
		friend bool TestAHealNamesTheNextCaptureAfresh(std::string* error);
		friend bool TestAStuckPrivateImageIsRetakenOnceThenRefused(std::string* error);
		friend bool TestWorldReturnWatchKeysOnWorldId(std::string* error);
		friend bool TestTheGoodbyeEndsWithItsRound(std::string* error);
		friend bool TestAnOwnSideErrorKeepsTheSeatsReconnect(std::string* error);
		/// Points the coordinator's handover at the service queue the pump drains. Caller holds the lock
		/// only where the match is already launched.
		void AttachCoordinatorSessionSink();
		/// Delivers the handover queue through the session before a teardown destroys the coordinator
		/// that filled it. Caller holds the lock. The census may only open where the sim stands at a
		/// completed tick with the world still up.
		void DrainPendingSessionEventsLocked(bool atTickBoundary);
		/// Keeps next-lobby packets until the rematch worker takes the link.
		void QueueLobbyEvent(const NetTransportEvent& event);
		/// Polls a finished session without touching the ended simulation; caller holds the lock.
		void PumpCompletedSessionLocked();
		/// Refuses Ready peers absent from the ended round; caller holds the lock.
		void RefuseEndedPeersLocked(const std::string& reason);
		void SayGoodbyeToRejoinersLocked();
		/// Reads a host's goodbye out of a session's refusal; true once one has been heard.
		bool NoteHostGoodbyeLocked(const NetSession* session);
		/// The relaunch's queue reset, with a permanent diagnostic for anything a teardown left behind.
		void DiscardUndeliveredSessionEventsLocked();
		/// Folds the coordinator's counters into the service so a gate can read them across a resync.
		void AccumulateLockstepTotalsLocked();
		/// Client: the §7 leave protocol, waiting exactly P21's budget for the ack before giving up and
		/// KEEPING the ticket. Runs only with a plane attached and a record to lose.
		void RunCleanLeave();
		/// The worker half of a leave: the §7 exchange, then - and only then - the round is told.
		void LeaveWorkerMain(std::string result);
		/// Ends the hosted session: tells every peer with the one reason that permits deleting a
		/// recovery record (P22), then clears the registry, the ledger and the seats. Caller holds the lock.
		void EndAdmissionSession();
		void ResetRosterTransitionHistory();
		void RecordRosterTransitions(uint64_t observedAtMs);
		/// Publishes a successful local host action to the presentation sink; caller holds the lock.
		void RecordModerationAction(uint16_t stableSeat, NetModerationAction action);
		/// Runs the §11 automatic-retry schedule from the service's own state. Game thread only.
		void DriveReconnectUx(uint64_t nowMs);
		bool PrepareHeldPeerRejoinLocked(uint8_t peerId);
		std::vector<NetGameReseat> m_PendingHeldReseats;
		std::vector<NetHoldResolutionNotice> m_PendingHeldResolutions;
		/// Destroys a rematch lobby whose peers did not all come back inside c_CompletedLobbyExpiryMs.
		/// Game thread only, from Update(): it takes the lock and then destroys without it.
		void UpdateCompletedLobbyExpiry(uint64_t nowMs);
		/// Whether every non-CPU seat of the current lobby is connected. Caller holds the lock.
		bool RematchLobbySeatedLocked() const;
		/// Elapsed milliseconds since this session began, for every admission deadline.
		uint64_t AdmissionNowMs() const;
		void CaptureA7SeatView();
		void CacheDiagnosticIdentity(const NetIdentityManifest& manifest);
		bool WaitForA7ConnectGate(std::string* error);
		/// Captures the round before its coordinator or activity is torn down. Caller holds the lock.
		void CaptureMatchSummaryLocked(const std::string& result);
		/// Counts public seat transitions independently of the bounded diagnostic history.
		void UpdateSummarySeatsLocked();


		mutable std::mutex m_Mutex;
		/// The thread driving a client's private world join under m_Mutex. A lobby message it handles can
		/// call back into the service, and a second lock from the owning thread is an error, not a wait.
		std::atomic<std::thread::id> m_LockedLobbyDriveThread;
		bool HoldsServiceLock() const { return m_LockedLobbyDriveThread.load(std::memory_order_acquire) == std::this_thread::get_id(); }
		/// Marks the calling thread as the one holding m_Mutex for a lobby drive, for as long as it lives.
		struct LockedLobbyDrive {
			std::atomic<std::thread::id>& owner;
			explicit LockedLobbyDrive(NetMatchService& service): owner(service.m_LockedLobbyDriveThread) { owner.store(std::this_thread::get_id(), std::memory_order_release); }
			~LockedLobbyDrive() { owner.store(std::thread::id{}, std::memory_order_release); }
			LockedLobbyDrive(const LockedLobbyDrive&) = delete;
			LockedLobbyDrive& operator=(const LockedLobbyDrive&) = delete;
		};
		std::string m_DiagnosticIdentity;
		NetIdentityManifest m_DiagnosticIdentityInputs; //!< The manager reads a captured build is waiting on.
		bool m_DiagnosticIdentityInputsPending = false;
		bool m_DiagnosticIdentityInputsWorld = false; //!< The target the inputs were captured for, so the build stamps the same versions.
		uint64_t m_DiagnosticIdentityGeneration = 0; //!< Bumped by every cached identity, so an older build knows it lost.
		uint64_t m_DiagnosticIdentityInputsGeneration = 0;
		std::string m_DiagnosticRuntimeError;
		static uint32_t s_AutosaveSeconds;
		static bool s_AutosaveSecondsOverridden;
		std::string m_AutosaveMatchId;
		uint32_t m_MatchAutosaveSeconds = 0; //!< The cadence the round agreed on, read once so the tick path never chases the runner.
		int64_t m_NextAutosaveSimTime = -1;
		int64_t m_LastAutosaveSimTime = -1;
		AutosaveIdentity m_AutosaveIdentity; //!< What every checkpoint of this match is stamped with.
		/// The agreed rewind point; a worker thread names it, and every capture in flight shares it so
		/// retention reads the live value instead of the one the capture started with.
		std::shared_ptr<AutosavePinSource> m_PinnedAutosave = std::make_shared<AutosavePinSource>();
		std::string m_RewindAnchorMatchId;
		uint64_t m_RewindAnchorTick = 0;
		bool m_RewindAnchorHeld = false;
		NetMatchServiceState m_State = NetMatchServiceState::Idle;
		std::string m_StatusText = "Idle";
		std::string m_ErrorText;
		std::string m_ActivityPreset;
		std::string m_ActivityModule;
		std::string m_SceneName;
		std::string m_SceneModule;
		std::thread m_Worker;
		std::jthread m_SnapshotLoadKeepalive;
		/// Host: the round's liveness ack, sent from off the simulation thread while that thread is busy, so a park of any length reads
		/// as a busy host, never a gone one. The session pump arms it every tick and it is disarmed around every transport change.
		struct HostLiveness {
			INetTransport* wire = nullptr;
			std::vector<NetPeerId> targets;
			std::vector<uint8_t> bytes;
			NetTransportLane lane = NetTransportLane::InputUnreliable;
			uint64_t pumpMs = 0, tickMs = 17, sentMs = 0, busyLimitMs = 0;
			uint64_t sentWhileBusy = 0;
		};
		std::mutex m_LivenessMutex;
		HostLiveness m_Liveness;
		std::jthread m_LivenessThread;
		void ArmHostLivenessLocked();
		void DisarmHostLiveness();
		std::atomic<uint64_t> m_SnapshotLoadKeepaliveTicks{0};
		std::atomic<uint64_t> m_SnapshotLoadKeepaliveWindowTicks{0};
		bool m_WorkerDone = false;
		bool m_IdentityPending = false;
		bool m_IsHost = false;
		uint8_t m_LocalPeerId = 0;
		int m_LocalTeam = -1;
		bool m_Dedicated = false;
		int m_HumanSeats = 0;
		NetMatchConfig m_MatchConfig; //!< The roster this peer asked for, until the round adopts the host's.
		NetMatchConfig m_AdoptedMatchConfig; //!< The lobby round's agreed config, mirrored each publish for the options view.
		std::optional<NetMatchConfig> m_PendingHostOptions; //!< Host-accepted options draft; the next match's intent.
		//!< The same draft on its way to the runner thread, which republishes it to every peer live.
		//!< Never read under m_Mutex by the runner: its own lock is all the slot needs.
		NetHostOptionsSlot m_HostOptionsRequest;
		bool m_ResyncOnDesync = false;
		std::string m_PendingResyncLoad;
		bool m_RoundStartScriptsWanted = false; //!< Whether the round this host sets up starts every peer on its script state.
		std::vector<uint8_t> m_RoundStartScripts; //!< The host's own start scripts for the round it sets up.
		std::vector<uint8_t> m_RoundStartScriptsToStream; //!< The same, until the worker streams them with the lobby start.
		std::vector<uint8_t> m_PendingRoundStartScripts; //!< The start scripts this peer lays onto its states when the round launches.
		std::optional<NetResyncState> m_PendingResyncState;
		/// The checkpoint the next launch loads out of this peer's own store instead of a received file.
		struct PendingAutosaveLoad {
			std::string matchId;
			uint64_t tick = 0;
		};
		std::optional<PendingAutosaveLoad> m_PendingAutosaveLoad;
		/// Host: the resume this run was started for, and the state it streams to peers that lack it.
		std::string m_ResumeMatchId;
		uint64_t m_ResumeTick = 0;
		std::string m_ResumeArchiveDigest;
		AutosaveSideState m_ResumeSideState;     //!< Host: the agreed state the checkpoint's manifest recorded.
		AutosaveSideState m_ResumeHeldSideState; //!< Client: the same, out of its own manifest.
		std::vector<uint8_t> m_ResumeAdmissionState;
		std::string m_ResumeDirectorySession, m_ResumeDirectoryToken;
		//!< Client: the checkpoint this peer answered the host's resume offer with.
		std::string m_ResumeHeldMatchId;
		uint64_t m_ResumeHeldTick = 0;
		uint64_t m_ResumeHeldRound = 0;
		uint64_t m_ResumeRoundId = 0;
		uint32_t m_ResumeIntervalSeconds = 0;
		std::vector<uint8_t> m_LastRestartAdmissionState;
		std::string m_PublishedDirectorySession, m_PublishedDirectoryToken;
		std::string m_PublishedAdmissionMatchId;    //!< The match the file on disk belongs to.
		uint64_t m_PublishedAdmissionRevision = 0;  //!< The admission plane's revision that file renders.
		NetDirectoryClient m_ReturnWatch; //!< 7e: browses for the watched session's row; never registers one.
		bool m_ReturnWatchConfigured = false;
		uint64_t m_RestartAdmissionGeneration = 0;
		std::atomic<bool> m_RestartAdmissionDue{false};
		bool m_FinalCheckpointWritten = false; //!< One final world checkpoint per teardown, never two.
		/// The checkpoint this world's round opened on, until the launch has recorded its segment. One
		/// shot: a later heal of the same round records the ordinary way.
		uint64_t m_ResumeSegmentTick = 0;
		bool m_ResyncRetainsLocalState = false;
		uint64_t m_ResyncSourceRound = 0;
		//!< The last host snapshot's tick label and the completed tick it was taken at; a gate asserts they match.
		std::atomic<uint64_t> m_ResyncSavedTick{UINT64_MAX};
		std::atomic<uint64_t> m_ResyncBoundaryTick{UINT64_MAX};
		std::string m_LocalName;
		NetLobbySnapshot m_LobbySnapshot;
		std::optional<NetMatchSummary> m_LastMatchSummary;
		NetMatchSummary m_CurrentMatchSummary;
		std::map<uint8_t, NetSeatPresenceEntry> m_SummarySeats;
		NetSeatAuthRegistry m_SeatAuth; //!< Hosted-session reconnect-auth material (off-sim epoch + seat credentials); survives resync/rejoin/rematch.
		// The admission plane lives on the service, not on a session or a match round, so a seat and its
		// ledger survive resync, rejoin and rematch exactly as the registry does (§3).
		NetReconnectHost m_ReconnectHost;
		NetReconnectClient m_ReconnectClient;
		NetReconnectTicketStore m_TicketStore;
		NetParticipantIdentityStore m_ParticipantStore;
		NetHostBanStore m_BanStore;
		NetReconnectUx m_ReconnectUx;
		NetSeatPresence m_SeatPresence;
		struct RosterTransition {
			uint8_t peerId = 0;
			std::string state;
			std::string line;
			uint64_t appliedFrame = 0;
			uint64_t observedAtMs = 0;
		};
		std::vector<RosterTransition> m_RosterTransitions;
		uint32_t m_RosterTransitionsDropped = 0;
		std::map<uint8_t, std::pair<std::string, std::string>> m_LastRosterPair;
		std::vector<NetH4ModerationSeat> m_ModerationSeats; //!< Immutable UI copy while a setup/resync worker owns the plane.
		NetKickBanResult m_LastKickBanResult = NetKickBanResult::NotHosting;
		NetParticipantRemovalIssue m_LastRemovalIssue;
		/// One host moderation action waiting for the setup worker: a removal, or an unban of an identity.
		struct PendingModeration {
			bool unban = false;
			NetModerationSelection selection;
			NetParticipantRemovalAction action = NetParticipantRemovalAction::Kick;
			NetAuthBytes32 identity{};
		};
		std::vector<PendingModeration> m_PendingModeration; //!< Starting-state kicks and unbans, in the order the host asked for them.
		std::vector<std::string> m_PendingToasts;      //!< Moderation lines a worker produced, for the game thread to show.
		/// Shows what a worker-side removal produced. Game thread only; never called under the lock.
		void PushPendingToasts();
		uint32_t m_LastRoundId = 0;                    //!< The round the peers last played; what a kick between rounds is stamped with.
		NetKickBanResult ApplyRemovalLocked(const NetModerationSelection& selection, NetParticipantRemovalAction action, NetSession& session);
		NetKickBanResult ApplyUnbanLocked(const NetAuthBytes32& identity);
		/// Queues a Starting-state action for the setup worker, or refuses a flood no lobby could produce.
		NetKickBanResult QueueModerationLocked(const PendingModeration& pending);
		/// Wires the setup worker's host pump. The runner calls it with the session it ticks.
		void AttachHostPump(NetMatchRunnerConfig& config);
		void DrainPendingModeration(NetSession& session);
		bool m_AdmissionAttached = false;
		bool m_LeaveExchangeRun = false; //!< The §7 exchange has been attempted for this session; Destroy must not repeat it.
		bool m_MatchWasRunning = false;  //!< This session reached a running match, so §11's recovery applies to losing it.
		bool m_LandedWithoutFrame = false; //!< The host dropped before this seat committed a frame: it landed with nothing to reclaim.
		bool m_FailedWithoutFrame = false; //!< The failed round's coordinator had simulated no frame when it was torn down.
		std::set<NetPeerId> m_HeldWorldReclaims; //!< Host: world joins returning to a seat the AI held, agreed by the reclaim itself.
		uint64_t m_LastUpdateMs = 0;     //!< The millisecond Update() last ran, so two callers in one frame do one pump.
		std::vector<NetH4SeatStatus> m_SeatStatuses; //!< Published from the sim pump for the roster (§11).
		std::string m_InputDelayText; //!< The announced input-delay line, built beside each lobby publish.
		std::atomic<uint32_t> m_CensusRefusals{0};   //!< Ownership censuses refused because the caller was not the sim thread.
		/// The largest the session clock has ever run ahead of the admission clock at a pump. Zero on a
		/// tree where they are one clock; the inflation itself on one where they are not, whenever the
		/// report is written - which the two clocks in the report cannot say, being read after teardown.
		std::atomic<uint64_t> m_MaxClockDivergenceMs{0};
		/// The moderator stand-in for the unattended gates: runs from PumpSessionEvents, on the game
		/// thread, and does exactly what a host clicking the UI would do.
		void DriveAutoSubstitution(uint64_t nowMs);
		/// Called with the service lock on the game thread, when it owns the admission plane.
		void PublishModerationView();
		/// The lobby's moderation rows, built on the setup worker under the service lock. Same seats the
		/// running view publishes, without the coordinator's frames - there is no coordinator yet.
		void PublishLobbyModerationViewLocked();
		uint64_t m_LobbyModerationSignature = 0; //!< The plane stamp the published lobby rows were built from.
		bool m_LobbyModerationPublished = false;

		static bool s_AdmissionEnabled;
		static std::string s_TicketStorePath;
		static std::string s_HostBanStorePath;
		static std::string s_JoinWaitPath;
		static bool s_ApplyForSeat;
		static uint16_t s_ApplySeat;
		static bool s_ApplyOnce; //!< The menu's one-shot application; consumed by the next join's plane.
		bool m_JoinRefusedByLiveMatch = false; //!< The last join was refused by a running match (§9b).
		static bool s_AutoSubstitute;
		static uint16_t s_AutoSubstituteSeat;
		static uint64_t s_AutoSubstituteDelayMs;
		static bool s_AutoSubstituteThenCancel;
		uint64_t m_AutoSubstituteReadyMs = 0; //!< When the stand-in first saw an applicant it could approve.
		bool m_AutoSubstituteDone = false;

		std::unique_ptr<GnsTransport> m_Transport;
		//!< ICE runs only; the direct-IP path keeps the plain transport above untouched.
		std::unique_ptr<NetMuxTransport> m_Mux;
#ifdef CCCP_WITH_GNS
		//!< The session directory's signal relay; pumped by the mux on the transport-owner thread.
		std::unique_ptr<GnsDirectorySignalDispatcher> m_Dispatcher;
#endif
		bool m_IceEnabled = false;          //!< This run offers (host) or takes (client) a session-id join.
		std::string m_IceBoundSessionId;    //!< The session id the process's GNS identity is pinned to.
		std::string m_IceIdentity;
		std::string m_IceJoinSessionId;     //!< Client: the session id -net-join-session named.
		std::string m_IceReport;            //!< The dispatcher's last report, taken when a worker or teardown takes the dispatcher.
		std::string m_IceRoute;             //!< The leg the join actually took: "ice" | "ip" | "".
		NetRelayConfig m_RelayOffer;
		/// A locked slot: libc++ has no atomic<shared_ptr>, and the pump reads this off the game thread.
		struct RelaySnapshotSlot {
			std::shared_ptr<const NetRelayConfig> load() const { std::lock_guard<std::mutex> lock(mutex); return value; }
			void store(std::shared_ptr<const NetRelayConfig> next) { std::lock_guard<std::mutex> lock(mutex); value = std::move(next); }
			mutable std::mutex mutex;
			std::shared_ptr<const NetRelayConfig> value;
		};
		RelaySnapshotSlot m_RelaySnapshot;
		NetRelayConfig m_FixedRelayOffer;
		std::string m_RelayError;
		int m_HostRelayMode = 1;
		int m_ConnectionMode = 0;
		bool m_RelayReady = false;
		bool m_RelayPublishPending = false;
		bool m_RelayAttempted = false;
		uint64_t m_RelayOfferIssuedAt = 0; //!< Wall seconds when the current offer was adopted.
		uint64_t m_RelayReplies = 0;
		uint64_t m_NextRelayRequestMs = 0;
		std::atomic<bool> m_FreshRelayRequested{true};
		//!< Published by Update() for the worker: the directory client is game-thread only.
		std::string m_DirectorySessionId;
		std::string m_DirectoryToken;
		bool m_DirectoryRegistered = false;
		std::unique_ptr<NetSession> m_Session;
		/// The session a rematch or resync worker owns; m_Session is empty for as long as it runs, and a
		/// park declared on this thread must still reach the session that is evaluating silence.
		NetSession* m_WorkerSession = nullptr;
		NetSession* LiveSessionLocked() const { return m_Session ? m_Session.get() : m_WorkerSession; }
		/// A park is the whole peer's, not one session's: a rejoin evaluates silence on the worker session while
		/// the old one is still held, so both hear it or the one that is counting times the rejoin out.
		void NotePumpParkedLocked() {
			if (m_Session) m_Session->NotePumpParked();
			if (m_WorkerSession && m_WorkerSession != m_Session.get()) m_WorkerSession->NotePumpParked();
		}
		/// The rejoin phase belongs to the peer, not to one session object: the worker and the live session are
		/// the same handshake seen from two threads.
		void SetRejoinPhaseLocked(NetSession::RejoinPhase phase);
		/// The rejoin's tail replay begins once the snapshot is loaded and the catch-up installed; it ends at the activation.
		void NoteTailReplayBeganLocked() { SetRejoinPhaseLocked(NetSession::RejoinPhase::TailReplay); }
		std::unique_ptr<NetLockstepCoordinator> m_Coordinator;
		std::unique_ptr<NetMatchRunner> m_Runner;
		std::vector<NetTransportEvent> m_PendingLobbyEvents;
		size_t m_PendingLobbyBytes = 0;
		bool m_PendingLobbyOverflow = false;
		bool m_LeftMatch = false;
		//!< Steady ms of the match end that opened this rematch lobby; 0 when no lobby is waiting.
		uint64_t m_CompletedLobbySinceMs = 0;
		uint64_t m_EndedLockstepPackets = 0;
		// Non-owning view of the live session object: while the runner's worker still owns it
		// (the whole lobby phase) m_Session is empty, but chat must already reach it.
		NetSession* m_ChatSession = nullptr;
		std::vector<NetTransportEvent> m_PendingSessionEvents; //!< Game-thread only: reconnect traffic the coordinator handed over.
		//!< Coordinator counters a resync would otherwise zero, accumulated at every teardown.
		struct LockstepTotals {
			uint64_t peerFramesWaived = 0;
			uint64_t peersDroppedSilent = 0;
			uint64_t connectionsClosedOnEviction = 0;
			std::map<uint8_t, NetLockstepPeerStats> peers;
		};
		LockstepTotals m_LockstepTotals;
		uint32_t m_SessionEventsDrained = 0;   //!< Handover events delivered by a teardown instead of the pump.
		uint32_t m_SessionEventsDiscarded = 0; //!< Handover events a relaunch found undelivered; must stay zero.
		NetAdmissionClock m_AdmissionClock; //!< One elapsed-time source for setup, play, stalls and resync.
		NetLanDiscovery m_LanDiscovery; //!< Game-thread only: the hosting lobby's LAN beacon.
		/// Game-thread only, like the beacon: the host's session-directory row. Unlike the beacon it
		/// stays listed while the match runs so a late joiner can still resolve it.
		NetDirectoryClient m_Directory;
		NetDirectoryRegisterRequest m_DirectoryRow; //!< The listing template; counts refresh per Update.
		bool m_DirectoryRetracted = false;          //!< The match ended while the state was still Running.
		bool m_DirectoryHidden = false;             //!< A natural ICE end keeps the bound row unlisted.
		bool m_DirectoryRelistPending = false;      //!< The next lobby awaits the hide acknowledgement.
		bool m_KeepEndedDirectoryLease = false;    //!< A held seat may still need the match-over answer.
		uint16_t m_BeaconGamePort = 0;
		uint8_t m_BeaconMaxPlayers = 2;
		std::atomic<bool> m_ReadyRequested{false};
		std::atomic<bool> m_StartRequested{false};
		std::atomic<bool> m_CancelRequested{false};
		std::atomic<bool> m_EverStarted{false};
		std::string m_CapturedRunnerReport;
		std::string m_RejoinOutcome;
		/// The frame the round ended on, set when this host says goodbye or this peer hears one; 0 while the
		/// round is live. The flag is separate because a goodbye may name no frame.
		uint64_t m_CompletedRoundFinalFrame = 0;
		bool m_HostGoodbyeSeen = false;
		/// Host: a seat was held when the round ended, so a rejoin still arriving is owed the goodbye.
		bool m_GoodbyeOwedToRejoiners = false;
		/// A goodbye belongs to the round it ended; the next round and a torn-down service start without one.
		void ResetRoundGoodbyeLocked() { m_CompletedRoundFinalFrame = 0; m_HostGoodbyeSeen = false; m_GoodbyeOwedToRejoiners = false; }
		struct LastResyncMetrics {
			uint64_t archiveBytes = 0;
			uint64_t envelopeBytes = 0;
			uint64_t saveMs = 0;
			uint64_t transferMs = 0;
			uint64_t healMs = 0;
			bool happened = false;
		};
		LastResyncMetrics m_LastResync;
		uint64_t m_ResyncHealStartMs = 0;
		bool m_ResyncHealOpen = false;
		bool m_HostRepairPending = false;
		bool m_HostRepairDeferred = false; //!< Host: a repair asked for while a returner catches up, started once it is back.
		uint64_t m_HostRepairDeferredMs = 0;
		bool m_HostLobbyBeaconed = false;
		NetWorldIdentity m_WorldIdentity;
		NetWorldJoinHost m_WorldJoin;
		int64_t m_WorldSpectatorsFree = 0; //!< The world's free watcher count, published for the directory row.
		// A capture the simulation queued and whose verdict the writer thread has not given yet.
		struct AwaitedAutosave { uint64_t tick = 0; bool joinCapture = false; };
		std::vector<AwaitedAutosave> m_AwaitedAutosaves;
		std::set<uint64_t> m_ScheduledCaptures; //!< Ticks the host named that this peer has not reached.
		uint64_t m_OpenCaptureTick = 0; //!< Host: the capture it named last, until every writer reported it.
		std::set<uint8_t> m_CaptureWriters; //!< Host: the peers still writing the open capture.
		bool m_OpenCaptureForJoin = false;
		uint64_t m_WorldCaptureRequestedTick = 0; //!< The tick a bootstrap already asked a capture at.
		bool m_WorldCapturePending = false;
		bool m_WorldSpectatorDeclinesPromotion = false; //!< This watcher's own choice, as it last sent it.
		bool m_LastJoinTargetPersistentWorld = false;
		std::optional<NetMatchServiceRequest> m_LastJoinRoute;
		bool BeginTicketRejoinOnRoute(std::string* error, const NetMatchServiceRequest* liveRoute);
		/// Held client: the hosts its rejoin may still find when its own is gone, in the match's published successor order.
		std::deque<NetMatchServiceRequest> m_HeldRejoinRoutes;
		uint64_t m_HeldRejoinPriorInput = 0;
		bool m_HeldRejoinDriving = false; //!< The held seat's rejoin loop owns the attempts until a launch or its last failure.
		uint32_t m_ReconnectRouteTurn = 0; //!< Alternates the reconnect prompt's attempts between the ticket's host and the successors.
		uint8_t m_ElectionHostPeer = 0; //!< Client: the round's host as last seen before an election.
		uint64_t m_HostSilenceAtElectionMs = UINT64_MAX; //!< Client: how long that host was quiet when its election began.
		/// A host heard this close to its election announced its leave; a lost one is silent for the host-silence bound (500 ms or more).
		static constexpr uint64_t c_HostAnnouncedSilenceMs = 250;
		bool m_RejoinOfRunningMatch = false; //!< Client: this service is rejoining the match it was playing, so its seat committed frames.
		bool m_HostEndedTheMatch = false; //!< Client: the host left a match this seat finished; it lands and nothing reconnects.
		/// Client: the host ended the match by leaving it; the ticket goes and no reconnect is offered or driven.
		void NoteHostEndedTheMatchLocked();
		NetWorldCatchUpClient m_WorldCatchUp;
		std::set<NetPeerId> m_PrivateActivations;
		std::map<NetPeerId, std::future<std::vector<uint8_t>>> m_PrivateJoinBlobs;
		std::deque<NetTransportEvent> m_CatchUpWirePackets;
		size_t m_CatchUpWireBytes = 0;
		std::unique_ptr<LoopbackTransport> m_CatchUpTransport;
		std::unique_ptr<NetLockstepCoordinator> m_CatchUpCoordinator;
		bool m_InPlaceCatchUp = false;   //!< Held client: the catch-up replays on its own state over its live connection.
		uint64_t m_InPlaceAskedMs = 0;   //!< When it last asked the host for its tail.
		uint64_t m_InPlaceSinceMs = 0;   //!< When it began; a host that never serves it sends it to the image path.
		uint64_t m_InPlaceHeardMs = 0;   //!< When its tail last moved.
		uint64_t m_InPlaceProgressApplied = 0;
		static constexpr uint64_t c_InPlaceHostSilenceMs = 3000; //!< A host that feeds a held seat nothing this long is gone.
		static constexpr uint64_t c_ReturnerReportGapMs = 500; //!< Host: a returner that has reported within this is still catching up.
		std::map<uint8_t, uint32_t> m_InPlaceIncarnationBumps; //!< Host: in-place returns per seat since its holder last bound, over the admission plane's count.
		std::map<uint8_t, std::string> m_RejoinFitReasons; //!< Host: the last reason each held seat's return was held back for.
		std::function<bool(Activity&)> m_ActivateCatchUpLocalSeat;
		struct PrivateJoinImage {
			NetWorldCheckpointImage image;
			std::shared_ptr<const std::vector<uint8_t>> archive;
			std::string error;
		};
		std::future<PrivateJoinImage> m_PrivateImageTask;
		bool m_PrivateBaseRequested = false; //!< Host: a returning seat's base is asked of the checkpoint schedule, which names its tick.
		uint64_t m_PrivateBaseTick = 0; //!< Host: the announced tick the base was taken at; its metadata is read at that tick's end.
		std::optional<NetWorldCheckpointImage> m_PrivateBasePending; //!< Host: the base read at its tick, waiting for its archive's writer.
		uint64_t m_PrivateImageRound = 0;
		uint64_t m_PrivateImageStaleFrom = 0; //!< Host: the frame a rejoin finished on; the base is older than play from here.
		uint64_t m_PrivateImageTakenMs = 0; //!< Host: when the base was last captured; the cadence is measured from it.
		double m_PrivateImageLastCaptureMs = 0.0; //!< Host: the last capture's measured cost.
		std::deque<double> m_PrivateCaptureCosts; //!< Host: the last three capture costs past the round's first, which the refresh rule reads.
		bool m_PrivateCaptureCold = false; //!< Host: the capture in flight is the round's first, whose one-time warm-up is not the steady cost.
		static constexpr uint64_t c_PrivateImageMinIntervalMs = 10000; //!< The shortest wall gap between two captures.
		static constexpr uint64_t c_PrivateImageWaitMs = 20000; //!< How long a returning seat waits on one capture's writer.
		bool m_PrivateImageRecapture = false; //!< Host: the next pass takes a fresh base; the stuck writer was abandoned.
		bool m_PrivateImageRecaptured = false; //!< Host: this wait already took its one fresh base.
		bool m_PrivateImageSeatHeld = false;
		std::map<NetPeerId, std::string> m_PrivateTransferHeldReasons; //!< Host: why a returner's image has not left, reported once per reason.
		const char* m_PrivateBaseHeldReason = nullptr; //!< Host: why the last pass could not take a base, reported once per reason.
		std::string m_PrivateJoinError;
		std::shared_ptr<const std::vector<uint8_t>> m_WorldJoinImageArchive; //!< The writer's own buffer, shared.
		std::string m_WorldJoinImageDigest;         //!< Its digest, so a stale cache is refused without a re-hash.
	};

} // namespace RTE
