#pragma once

#include "NetLobbyProtocol.h"
#include "NetLockstep.h"
#include "NetMatchConfig.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace RTE {

	/// A world's durable identity. The UUID is created once and never changes: it is also the world's
	/// directory registration id, and the ICE identity is derived from it. The boot incarnation is
	/// advanced and made durable BEFORE the host listens, so nothing a previous boot signed is live.
	struct NetWorldIdentity {
		std::string worldId;    //!< Canonical lowercase UUID; the directory registration id too.
		uint64_t boot = 0;      //!< Host boot incarnation, advanced before the first listen of this process.
		uint64_t round = 0;     //!< The round this boot opened; a restart opens a new one.
		std::string directoryToken; //!< The directory row's current token, so a rebooted host resumes its row.
		std::array<uint8_t, NetMatchConfigUtil::c_WorldTeamCount> teamCapacity{}; //!< Human seats per team; all zero when none was authored.
		uint8_t maxSpectators = 0;         //!< Authority-free watchers the world admits past capacity.
		uint16_t respawnDelaySeconds = 0;  //!< Seconds before a dead seat's brain is respawned.

		bool IsValid() const { return NetMatchConfigUtil::IsWorldId(worldId) && boot != 0; }
		bool operator==(const NetWorldIdentity&) const = default;
	};

	/// Whether the config names a world capacity. All zero is a host that authored none, and every
	/// capacity rule then keeps the pre-capacity behaviour instead of reading an unset field.
	inline bool WorldCapacityAuthored(const NetMatchConfig& config) {
		for (const uint8_t capacity: config.worldTeamCapacity) {
			if (capacity != 0) {
				return true;
			}
		}
		return false;
	}

	/// Whether the record names a capacity, so a later boot offers the same world instead of re-deriving one.
	inline bool WorldIdentityCarriesCapacity(const NetWorldIdentity& identity) {
		for (const uint8_t capacity: identity.teamCapacity) {
			if (capacity != 0) {
				return true;
			}
		}
		return false;
	}

	/// The world identity record on disk. The two durable numbers a boot must advance before it listens.
	class NetWorldIdentityFile {
	public:
		/// Reads the record, or writes a fresh one with a new UUID. Advances and flushes the boot
		/// incarnation before returning, so a caller that listens afterwards cannot reuse a boot.
		/// @param path The record's file path.
		/// @param out The identity this boot runs under.
		/// @return Whether the record could be read or created and the advanced boot made durable.
		static bool OpenForBoot(const std::string& path, NetWorldIdentity& out, std::string* error = nullptr);
		/// Reads without advancing anything; an absent record is not an error and leaves out empty.
		static bool Peek(const std::string& path, NetWorldIdentity& out, std::string* error = nullptr);
		/// Rewrites the record in place (temp + rename). The host uses it when the directory issues a
		/// new row token, so the token outlives the process that earned it.
		static bool Write(const std::string& path, const NetWorldIdentity& identity, std::string* error = nullptr);
		/// Renders the record. Exposed so a self-test can round-trip it without touching a disk.
		static std::string Encode(const NetWorldIdentity& identity);
		static bool Decode(const std::string& text, NetWorldIdentity& out, std::string* error = nullptr);
		/// A canonical lowercase random UUID (version 4), from the deterministic-session-free entropy source.
		static std::string MakeWorldId();
		/// The host's durable identity record. Created on first boot; later boots only advance it.
		static std::string DefaultPath();
	};

	/// Where a joining connection stands between its first authenticated packet and the tick its
	/// Controllers become required. Only Active is a producer the world waits for; nothing before it
	/// may enter the dropped-seat hold set or the quorum.
	enum class NetWorldJoinPhase : uint8_t {
		Idle = 0,
		Authenticating = 1,
		SnapshotTransfer = 2,
		CatchingUp = 3,
		Spectating = 4,
		Active = 5,
		Failed = 6,
	};

	const char* NetWorldJoinPhaseName(NetWorldJoinPhase phase);

	/// The immutable checkpoint image a join is bootstrapped from: frozen at a COMPLETED tick, never
	/// partly applied, and reused by every joiner inside its window rather than recaptured per joiner.
	struct NetWorldCheckpointImage {
		uint64_t privateSessionId = 0;
		std::string checkpointConfig;
		std::string sideState;
		NetLockstepPauseState pauseState;
		std::string heldState;
		std::string roundConfigHash;
		uint64_t authorityGeneration = 0;
		uint8_t authorityPeerId = 1;
		std::map<uint8_t, uint64_t> departedPeers;
		std::string worldId;
		uint64_t boot = 0;
		uint64_t round = 0;
		uint64_t tick = 0;                //!< B, the completed tick this image froze at.
		uint64_t configRevision = 0;
		uint64_t membershipRevision = 0;
		std::string matchConfigHash;
		std::string moduleManifestHash;
		std::string digest;               //!< Content digest of the published archive.
		std::string path;                 //!< Where the archive was published.
		uint64_t bytes = 0;
		double captureMs = 0.0;           //!< The sim-thread interruption this image cost.

		bool IsValid() const { return bytes != 0 && (privateSessionId != 0 ? worldId.empty() && round != 0 && !checkpointConfig.empty() && !sideState.empty() : NetMatchConfigUtil::IsWorldId(worldId) && boot != 0 && tick != 0); }
	};

	class NetCatchUpHeadroom {
	public:
		bool Observe(uint64_t ticks, uint64_t workUs, double tickMs);
		bool Ready() const { return m_Ready; }
		double Ratio() const { return m_Ratio; }
		/// Whether the replay measured at least this many ticks of work, and ran them faster than the round.
		/// @param ticks The ticks of work the measure must span.
		/// @return Whether it did.
		bool AboveRateOver(uint64_t ticks) const { return m_Samples.back().first - m_Samples.front().first >= ticks && m_Ratio > 1.0; }
	private:
		std::deque<std::pair<uint64_t, uint64_t>> m_Samples{{0, 0}};
		bool m_Ready = false;
		double m_Ratio = 0;
	};

	/// The offer a joiner is sent with the image: world/boot/round, B, digest and the lead to E.
	/// It is not a lobby version bump; only the joining connection reads it.
	std::string EncodeWorldJoinOffer(const NetWorldCheckpointImage& image);
	bool DecodeWorldJoinOffer(const std::string& text, NetWorldCheckpointImage& out, std::string* error = nullptr);

	/// One joining connection's bootstrap. The live coordinator keeps sole ownership of transport
	/// polling; this is the connection's state, never a socket.
	struct NetWorldJoinSession {
		NetPeerId connection = c_InvalidNetPeerId;
		NetWorldJoinPhase phase = NetWorldJoinPhase::Idle;
		uint16_t stableSeat = 0;
		std::string holderName;      //!< The name the admission plane gave this connection.
		uint32_t holderGeneration = 0;
		uint32_t incarnation = 0;
		uint64_t priorInputThrough = 0;
		bool linkFits = false;
		NetCatchUpHeadroom headroom;
		bool returnsToHeldSeat = false; //!< A world member taking back the seat held for it: it proves headroom as a private return does.
		const char* activationHeldReason = nullptr; //!< Why the last progress report did not activate it, reported once per reason.
		bool declinesPromotion = false; //!< A watcher that asked to stay one; promotion skips it.
		bool promoted = false;          //!< It reached its slot by promotion, not by a fresh join.
		uint64_t joinOrder = 0;         //!< Monotonic open order, so promotion takes the oldest watcher.
		uint8_t assignedPeerId = 0;       //!< The lockstep id activation will install; 0 until the host picks one.
		int8_t team = -1;
		bool spectator = false;
		uint64_t snapshotTick = 0;        //!< B.
		uint64_t activationTick = 0;      //!< E, announced before it arrives; 0 until scheduled.
		uint64_t acknowledgedActivation = 0;
		bool activationProposed = false;
		bool activationCommitted = false;
		uint64_t lastReannounceProgress = 0;
		std::vector<uint8_t> pendingTail;
		size_t pendingTailOffset = 0;
		uint64_t pendingTailThrough = 0;
		uint64_t deliveredThrough = 0;    //!< The last tail frame this connection has been sent.
		std::optional<uint64_t> finalTailFrame;
		std::optional<uint64_t> finalTailRewind;
		uint64_t acknowledgedThrough = 0; //!< The last tail frame it says it applied.
		uint64_t openedAtMs = 0;
		uint64_t transferBytes = 0;
		uint64_t transferId = 0;          //!< The StateChunk transfer the joiner is receiving.
		uint16_t ackedChunks = 0;
		uint16_t totalChunks = 0;
		uint32_t activationReannounces = 0; //!< At most one later E; then the slot is freed.
		bool transferStarted = false;
		bool matchConfigSent = false;     //!< The seat's config went out once; a retry does not resend it.
		uint64_t catchUpTicks = 0;        //!< Ticks it reported replaying, for the catch-up rate.
		uint64_t catchUpMs = 0;
		uint64_t wallCatchUpTicks = 0;   //!< Ticks replayed between timed reports, for the wall-clock catch-up rate.
		uint64_t wallCatchUpMs = 0;
		uint64_t lastCatchUpReportMs = 0; //!< Host clock of the last catch-up report, for elapsed.
		uint64_t catchUpSinceMs = 0; //!< Host clock of the first catch-up report; a returner's headroom is judged over the time since.
		uint64_t atHeadSinceFrame = 0; //!< The applied frame from which it has replayed within the lead of the round's horizon; 0 while behind.
		uint64_t closingAnchorApplied = 0, closingAnchorHorizon = 0; //!< The report its closing rate on the round is measured from.
		double closingRate = 0.0; //!< Frames its replay gains on the round per frame the round commits, once measured.
		bool closingMeasured = false;
		uint64_t activationTrailFrames = 0; //!< How far it trailed the round when its activation was announced: its return leaves it that long.
		uint32_t windowsWithoutProgress = 0; //!< Closing windows its replay stood still through, in a row.
		struct TailDatagram { uint64_t first = 0, last = 0, firstSentMs = 0, sentMs = 0; bool repeated = false; };
		std::deque<TailDatagram> tailInFlight; //!< Tail datagrams sent and not yet passed by its replay, lowest frames first.
		double tailAckRttMs = 0; //!< The shortest time from a datagram's first send to the report that passes it; 0 until measured.
		uint32_t tailLinkRttMs = 0; //!< The link's own round trip to the returner, as its transport measures it; 0 until known.
		uint64_t tailSentNew = 0, tailSentRepeat = 0, tailSentResend = 0, tailSentBytes = 0, tailRefused = 0, tailLoggedMs = 0; //!< The tail's traffic, logged every two seconds.
		const char* catchUpGate = nullptr; //!< What the last catch-up report met on its way to an activation.
		const char* catchUpGateLogged = nullptr; //!< The gate last written to the log, and the horizon it was written at.
		uint64_t catchUpGateLoggedFrame = 0;
		uint8_t spectatorLobbyPeer = 0;   //!< Non-member lobby id in [32, 47]; 0 if none remains.
		std::string refusal;              //!< Why the bootstrap failed; empty while it is alive.
		uint64_t lagSinceTick = 0, lagSinceTrail = 0; //!< Where its trail last stood past the history's lag limit without gaining.
	};

	/// The bounded log of canonical committed frames from B+1 onward. A joiner applies these at the
	/// fixed timestep after restoring the image, so the world never waits for it.
	class NetWorldFrameLog {
	public:
		static constexpr size_t c_DefaultMaxFrames = 3600;              //!< A minute of 60 Hz ticks.
		static constexpr uint64_t c_DefaultMaxBytes = 32ULL * 1024 * 1024;
		static constexpr size_t c_JournalSegmentFrames = 3600;          //!< The journal's frames per file; the oldest go a file at a time.
		static constexpr uint64_t c_JournalQueueBytes = 32ULL * 1024 * 1024; //!< Frames waiting for the journal's writer; past it the journal fails.
		static constexpr uint64_t c_JournalReopenFrames = 600;          //!< The first wait before a failed journal is opened again; it doubles per failure.

		/// The frames a peer's record of its round keeps: the slow-player bound, the delay margin and one capture interval of frames.
		static size_t RingFrames(uint32_t boundTicks, uint32_t delayMarginFrames, uint64_t captureIntervalMs, double tickMs);
		void Configure(size_t maxFrames, uint64_t maxBytes);
		void EnableJournal(const std::string& path);
		bool HasJournal() const { return static_cast<bool>(m_Journal); }
		/// Keeps the journal to its newest frames whatever its readers need, a whole file at a time: it never holds more than this many
		/// frames plus one file. 0 leaves it to PruneJournalBefore alone.
		void SetJournalRetention(uint64_t frames) { m_JournalRetain = frames; }
		uint64_t JournalRetention() const { return m_JournalRetain; }
		/// The most frames the journal ever holds: its retention plus the file being written. 0 while no retention bounds it.
		uint64_t JournalBoundFrames() const { return m_JournalRetain == 0 ? 0 : m_JournalRetain + c_JournalSegmentFrames; }
		/// Why the journal stopped taking frames; empty while it works.
		std::string JournalFailure() const;
		/// Replaces a failed journal with a fresh one beside it, seeded with the frames still in memory. Refuses until its retry frame,
		/// which doubles from c_JournalReopenFrames with every failure, so a disk that keeps failing is not reopened every pass.
		bool ReopenJournal(uint64_t nowFrame);
		uint32_t JournalReopens() const { return m_JournalReopens; }
		/// Why a failed journal stopped, once per journal; false while it works and once it has been told.
		bool TakeJournalFailure(std::string& why);
		/// The journal file being written, without its folder.
		std::string JournalFileName() const;
		/// The journal's files on disk as its writer last left them, and the frames it covers.
		struct JournalStats {
			uint64_t bytes = 0;
			uint32_t files = 0;
			uint64_t first = 0, last = 0;
			uint64_t indexBytes = 0;      //!< Memory: the files' per-frame offsets.
			uint32_t cachedReads = 0;     //!< Memory: the reads kept for the joiners asking again.
			uint64_t cachedReadBytes = 0;
		};
		JournalStats GetJournalStats() const;
		size_t MaxFrames() const { return m_MaxFrames; }
		uint64_t MaxBytes() const { return m_MaxBytes; }
		bool JournalFailed() const;
		/// Encodes and retains one committed frame. Frames must arrive in order and without gaps, and of the log's round once it has one.
		bool Append(const NetLockstepFrame& frame, std::string* error = nullptr);
		/// Keys the log to one round: a frame of any other round is refused. 0 leaves it unkeyed; Clear unkeys it.
		void SetRound(uint64_t round) { m_Round = round; }
		uint64_t Round() const { return m_Round; }
		/// The oldest frame the log can still serve, from its journal or its memory; 0 when it holds none.
		uint64_t FirstServableFrame() const;
		/// Whether the log still covers the frame, so a join opened at B-1 can still converge.
		bool Covers(uint64_t frame) const;
		uint64_t FirstFrame() const { return m_Records.empty() ? 0 : m_Records.front().frame; }
		uint64_t LastFrame() const { return m_Records.empty() ? 0 : m_Records.back().frame; }
		size_t Count() const { return m_Records.size(); }
		uint64_t Bytes() const { return m_Bytes; }
		uint64_t Evicted() const { return m_Evicted; }
		/// Copies records from `from` onward, bounded by both counts, for one bulk pump.
		/// lastCopied is the last record.frame that entered out, or 0 if none did.
		size_t CopyFrom(uint64_t from, size_t maxRecords, uint64_t maxBytes, std::vector<std::vector<uint8_t>>& out, uint64_t* lastCopied = nullptr) const;
		/// Forgets everything at or before the frame every live bootstrap has applied.
		void DropThrough(uint64_t frame);
		/// Lets the journal go below the oldest frame anyone may still be served from, a whole file at a time.
		void PruneJournalBefore(uint64_t frame);
		/// Takes another log's records as this empty log's own, bounded as this log is. False when this log already holds a record
		/// or one of them belongs to another round than this log's.
		bool AdoptRecords(const NetWorldFrameLog& other);
		void Clear();

	private:
		struct Record {
			uint64_t frame = 0;
			uint64_t round = 0;
			std::vector<uint8_t> bytes;
		};

		void Trim();
		void OpenJournal(const std::string& path, bool reopened);
		struct Journal;
		std::shared_ptr<Journal> m_Journal;
		std::vector<std::shared_ptr<Journal>> m_RetiredJournals; //!< Failed journals told to stop, let go once their writers have.
		uint64_t m_JournalBase = 0, m_JournalFirst = 0, m_JournalLast = 0; //!< Base: its first file's first frame, where every file boundary is counted from.
		std::string m_JournalPath;      //!< The path the round's journal was enabled at; a reopened one is written beside it.
		uint64_t m_JournalRetain = 0;
		uint32_t m_JournalReopens = 0;
		uint64_t m_JournalReopenAt = 0; //!< The first frame a failed journal may be opened again at.
		bool m_JournalFailureTaken = false;

		std::deque<Record> m_Records;
		size_t m_MaxFrames = c_DefaultMaxFrames;
		uint64_t m_MaxBytes = c_DefaultMaxBytes;
		uint64_t m_Bytes = 0;
		uint64_t m_Evicted = 0;
		uint64_t m_Round = 0;
	};

	/// Baselines recorded as the world runs and reported at its end. A missed bound is reported, never rounded away.
	class NetWorldMetrics {
	public:
		static constexpr size_t c_MaxSamples = 4096;
		/// The one-tick ceiling a capture is measured against (60 Hz). A miss is reported, not clamped.
		static constexpr double c_CaptureCeilingMs = 1000.0 / 60.0;

		void NoteCapture(double stallMs, uint64_t bytes);
		void NoteCatchUp(uint64_t ticks, uint64_t elapsedMs);
		void NoteTransfer(uint64_t bytes);
		/// A bootstrap that could not be started at all, so the world ended it instead of retrying.
		void NoteBootstrapStall();
		/// Records one tick hashed with and without a capture. No capture path produces that pair yet.
		void NotePurity(uint64_t tick, uint64_t withCapture, uint64_t withoutCapture);

		double CapturePercentileMs(double percentile) const;
		double CaptureMaxMs() const { return m_CaptureMaxMs; }
		uint64_t Captures() const { return m_Captures; }
		uint64_t CaptureCeilingMisses() const { return m_CaptureCeilingMisses; }
		double CatchUpRatio() const; //!< Joiner ticks per world tick; must exceed 1 to converge.
		uint64_t BootstrapStalls() const { return m_BootstrapStalls; }
		uint64_t PurityProbes() const { return m_PurityProbes; }
		uint64_t PurityMismatches() const { return m_PurityMismatches; }
		std::string BuildReportJson() const;
		void Reset();

	private:
		std::vector<double> m_CaptureStalls;
		uint64_t m_Captures = 0;
		uint64_t m_CaptureBytes = 0;
		double m_CaptureMaxMs = 0.0;
		uint64_t m_CaptureCeilingMisses = 0;
		uint64_t m_CatchUpTicks = 0;
		uint64_t m_CatchUpMs = 0;
		uint64_t m_TransferBytes = 0;
		uint64_t m_BootstrapStalls = 0;
		uint64_t m_PurityProbes = 0;
		uint64_t m_PurityMismatches = 0;
		uint64_t m_FirstPurityMismatchTick = 0;
	};

	/// One configured gameplay slot of a persistent world.
	struct NetWorldSlot {
		uint8_t peerId = 0;        //!< The lockstep id this slot always uses; stable for the world's life.
		int8_t team = -1;          //!< The team the host assigns in the configured order.
		uint32_t generation = 0;   //!< Advanced on every clean leave, so a returner is a new holder.
		uint16_t stableSeat = 0;   //!< The admission seat bound to the slot while it is held.
		bool seated = false;       //!< A seat has been bound to the slot; seat 0 is a seat, an ordinary match's original host's.
		bool held = false;
		bool reclaimHold = false; //!< Its holder dropped: only that holder may take it back.
		uint64_t brainMissingSince = 0; //!< The committed frame its brain went; 0 while one lives.
		uint64_t respawnScheduledAt = 0; //!< The frame the last respawn was authored for.
		std::string holderName;
	};

	/// The host's slot table. The order is configured, never derived from a live peer count: a clean
	/// leave frees its slot under a new generation, and a credentialed reclaim outranks a fresh join.
	class NetWorldMembership {
	public:
		/// Builds the fixed order from the world's config: one slot per non-host peer id, teams in the
		/// config's team order. Called once at boot and after a restart, never mid-round.
		bool Configure(const NetMatchConfig& config, std::string* error = nullptr, bool privateMatch = false);
		/// The slot a fresh join takes: the first free one in the configured order that no reclaim
		/// hold is keeping. Null when the world is full, which makes the joiner a spectator.
		const NetWorldSlot* FirstFreeSlot() const;
		/// The slot a credentialed holder reclaims; null when the seat is not this world's.
		const NetWorldSlot* SlotOfSeat(uint16_t stableSeat) const;
		/// The slot that plays this lockstep id; null when the id is not one of this world's slots.
		const NetWorldSlot* SlotOfPeer(uint8_t peerId) const;
		bool Hold(uint8_t peerId, uint16_t stableSeat, const std::string& holderName, std::string* error = nullptr);
		/// Gives a slot back to the holder its seat names. The generation does not move: a reclaim
		/// is the same holder returning, not a new one, so the credentials it holds stay good.
		bool Reclaim(uint8_t peerId, uint16_t stableSeat, const std::string& holderName, std::string* error = nullptr);
		/// Marks the slot as waiting for its dropped holder, so no fresh join may allocate it.
		bool SetReclaimHold(uint8_t peerId, bool holding);
		/// Records whether the seat's brain is alive at this committed frame. The first frame with
		/// none starts the seat's respawn clock; a living brain clears it.
		bool NoteSeatBrain(uint8_t peerId, bool alive, uint64_t nowFrame);
		/// Records that a respawn has been authored for the seat, so one death spawns one brain.
		bool NoteSeatRespawn(uint8_t peerId, uint64_t atFrame);
		/// The seat whose brain has been gone for the whole delay and has no respawn out yet.
		const NetWorldSlot* DueSeatRespawn(uint64_t nowFrame, uint64_t delayFrames) const;
		/// Whether a reclaim hold is keeping this slot for its holder right now.
		bool HoldsForReclaim(uint8_t peerId) const;
		/// Frees the slot and advances its generation. A later return of the same player is a fresh
		/// admission: the world never silently promises back the character it had.
		bool Release(uint8_t peerId, std::string* error = nullptr);
		const std::vector<NetWorldSlot>& Slots() const { return m_Slots; }
		size_t FreeSlots() const;
		/// Slots a reclaim hold is keeping; they are held, so they are not free either.
		size_t ReclaimHolds() const;
		size_t HeldSlots() const;
		uint64_t Revision() const { return m_Revision; }
		std::string BuildReportJson() const;

	private:
		NetWorldSlot* Find(uint8_t peerId);

		std::vector<NetWorldSlot> m_Slots;
		uint64_t m_Revision = 1;
	};

	/// How far ahead of the tail the host announces an activation, so every peer installs the new
	/// member's generation and sequence baseline before its first required frame.
	inline constexpr uint64_t c_NetWorldActivationLeadFrames = 60;
	/// A bootstrap that has not converged by here is cancelled and rebased onto a newer image, so an
	/// endless transfer cannot pin the world's memory.
	inline constexpr uint64_t c_NetWorldJoinDeadlineMs = 180000;
	/// One later E if the joiner is still behind when the first E arrives; a second miss frees the slot.
	inline constexpr uint32_t c_NetWorldActivationReannounceLimit = 1;
	/// A returner that replays within the activation lead of the round for this many ticks keeps the round's pace: the tail it
	/// replays arrives at that pace, so its measured rate cannot exceed the round's and is no evidence of a slow machine.
	inline constexpr uint64_t c_NetWorldPaceProofTicks = 120;
	/// The round frames a returner's closing rate is measured over.
	inline constexpr uint64_t c_NetWorldClosingWindowFrames = 20;
	/// Closing windows a returner's replay may stand still through before it is taken for parked and told so.
	inline constexpr uint32_t c_NetWorldNoProgressWindows = 3;
	/// A tail datagram's whole frames: few enough that one lost packet costs only them.
	inline constexpr size_t c_NetWorldTailDatagramFrames = 4;
	inline constexpr uint64_t c_NetWorldTailDatagramBytes = 1000;
	/// Every tail datagram goes a second time this long after its first, so one lost packet costs its frames nothing.
	inline constexpr uint64_t c_NetWorldTailRepeatMs = 20;
	/// How many of the lowest unpassed tail datagrams go again when their resend time runs out.
	inline constexpr size_t c_NetWorldTailResendDepth = 2;
	/// The datagrams in flight to one returner: a replay far behind passes them late, and new frames must not wait on it.
	inline constexpr size_t c_NetWorldTailInFlightLimit = 8192;
	/// How long a returning seat may replay without showing headroom before its rejoin is ended and retried.
	inline constexpr uint64_t c_NetWorldHeadroomWaitMs = 30000;
	/// World-join plane schema on the offer, the transition and the membership report.
	inline constexpr uint16_t c_NetWorldJoinSchema = 1;
	/// Overflow spectators bind lobby ids in [first, last], one per connection, above member seats.
	inline constexpr uint8_t c_WorldSpectatorLobbyPeerFirst = NetLobbyProtocol::c_FirstWatcherPeer;
	inline constexpr uint8_t c_WorldSpectatorLobbyPeerLast = NetLobbyProtocol::c_LastWatcherPeer;
	inline constexpr size_t c_WorldSpectatorLobbyCap = static_cast<size_t>(c_WorldSpectatorLobbyPeerLast - c_WorldSpectatorLobbyPeerFirst + 1);
	// A host may never configure more spectators than the world has lobby ids to bind them on.
	static_assert(NetMatchConfigUtil::c_MaxWorldSpectators <= c_WorldSpectatorLobbyCap);
	// The id a refusal is answered on. Binding re-points a known remote's transport, so a refusal
	// answered on a watcher's id would hand that watcher's stream to the connection being refused.
	inline constexpr uint8_t c_WorldRefusalLobbyPeer = c_WorldSpectatorLobbyPeerLast + 1;
	static_assert(c_WorldRefusalLobbyPeer > c_WorldSpectatorLobbyPeerLast);
	static_assert(c_WorldRefusalLobbyPeer < c_WorldSpectatorLobbyPeerFirst || c_WorldRefusalLobbyPeer > c_WorldSpectatorLobbyPeerLast);
	// Above every lockstep peer id a member can take, so it is never a seat's id either.
	static_assert(c_WorldRefusalLobbyPeer > NetLockstepCodec::c_MaxPeerCount);
	static_assert(c_WorldRefusalLobbyPeer != 0);

	/// How many watchers a world admits past its seats. The one rule: a host that authored no capacity
	/// keeps the lobby-id pool it offered before, and an authored bound is held under that pool.
	inline size_t WorldSpectatorBound(const NetMatchConfig& config) {
		if (!WorldCapacityAuthored(config)) {
			return c_WorldSpectatorLobbyCap;
		}
		return std::min<size_t>(config.worldMaxSpectators, c_WorldSpectatorLobbyCap);
	}

	/// A joiner replaying toward its activation, or a watcher replaying for as long as it watches.
	inline bool StreamsTail(const NetWorldJoinSession& session) {
		return session.phase == NetWorldJoinPhase::CatchingUp || (session.spectator && session.phase == NetWorldJoinPhase::Spectating);
	}

	/// The host's policy for a join plane's round history, in frames: the play its journal keeps whatever any reader needs, and how far a
	/// replaying reader no join deadline bounds (a watcher, a returning seat of a match) may trail the round before its bootstrap ends.
	/// Zero turns either off.
	struct NetJoinHistoryPolicy {
		uint64_t retainFrames = 0;
		uint64_t lagLimitFrames = 0;
		uint8_t returnWindowMinutes = NetMatchConfigUtil::c_DefaultReturnWindowMinutes; //!< The agreed window a held seat may still come back in place within.
	};

	/// A bootstrap the round's history stopped serving, and the receipt that says why.
	struct NetJoinHistoryEnd {
		NetPeerId connection = c_InvalidNetPeerId;
		bool rebase = false; //!< A returning seat of a match: it comes back on a fresh image. Any other reader is closed.
		std::string receipt;
	};

	inline uint8_t WorldJoinLobbyPeer(const NetWorldJoinSession& session) {
		if (session.assignedPeerId != 0) {
			return session.assignedPeerId;
		}
		return session.spectatorLobbyPeer;
	}

	/// Where the round changed hands: the first frame the new authority committed, its generation and id, and the peers that
	/// left at that frame (bit n-1 for peer n). A returner replaying across it replays each side under its own authority.
	struct NetWorldHandover {
		uint64_t frame = 0;
		uint64_t generation = 0;
		uint8_t authorityPeerId = 0;
		uint8_t departedMask = 0;
		bool operator==(const NetWorldHandover&) const = default;
	};

	/// The joiner's own bootstrap state: the image it restored, the tail it holds and the E it was given.
	struct NetWorldCatchUpClient {
		bool active = false;
		uint64_t tailDatagrams = 0, tailFramesKept = 0, tailFramesRepeated = 0; //!< What its tail brought, for its progress line.
		uint64_t reportsSent = 0, reportsRefused = 0, reportsLogged = 0; //!< The progress reports it sent, those its wire refused, and the applied frame last logged.
		uint64_t reportAttemptMs = 0;
		std::vector<uint8_t> lastReportBytes;
		bool privateMatch = false;
		NetMatchConfig checkpointConfig;
		std::string sideState;
		NetLockstepPauseState pauseState;
		std::map<uint8_t, NetGameSeatHold> initialHolds;
		std::map<uint8_t, NetGameSeatReclaim> initialReclaims; //!< Returns taken at or before B, whose neutral gap may run past it.
		uint64_t roundId = 0, authorityGeneration = 0;
		uint8_t authorityPeerId = 1;
		std::map<uint8_t, uint64_t> initialPeerLeaves;
		NetHash32 roundConfigHash{};
		uint64_t snapshotTick = 0;        //!< B, the tick the restored image froze at.
		uint64_t appliedThrough = 0;      //!< The last committed tail frame the sim has applied.
		uint64_t activationTick = 0;      //!< E, once the host has announced it.
		bool activationCommitted = false;
		std::string digest;
		std::optional<uint64_t> endRecord;
		std::vector<NetLockstepFrame> tail;
		std::vector<uint8_t> partialTail;
		std::set<uint64_t> droppedForeignRounds; //!< Rounds whose stray tail chunks this catch-up dropped, each named once.
		std::optional<NetWorldHandover> handover; //!< Where the round it replays changed hands, once its successor said so.
		bool handoverCrossed = false;             //!< The replay runs under the successor's authority from the handover on.
		bool handedToRound = false;               //!< The replay gave the sim to the live round at its activation; nothing replays after it.
	};

	/// The catch-up report a joiner sends: what its sim has applied, never the host's frame.
	NetLobbyStateChunk MakeJoinerCatchUpReport();
	/// WJIM: the joiner-only checkpoint envelope streamed through the lobby StateChunk pump.
	inline constexpr uint32_t c_NetWorldImageMagic = 0x4D494A57U;
	/// 3: a world offer carries the seats of its tick (held_state, departed_peers, the authority) beside its lockstep state.
	inline constexpr uint8_t c_NetWorldImageVersion = 3;
	/// A valid 9-byte StateChunk the joiner and host exchange for progress, catch-up and E.
	inline constexpr uint64_t c_NetWorldReportTransferId = 0x574A5250ULL;
	inline constexpr uint64_t c_NetWorldTailTransferId = 0x5441494CULL;
	/// A tail chunk leads with the round its frames belong to, so a chunk of an earlier round is never replayed into a later one.
	inline constexpr size_t c_NetWorldTailRoundBytes = 8;
	/// A frame that does not fit one lobby chunk cannot be a datagram: it goes on the ordered lane in pieces.
	inline constexpr uint64_t c_NetWorldTailDatagramFrameLimit = NetLobbyProtocol::c_MaxStateChunkBytes - c_NetWorldTailRoundBytes - 4;
	NetLobbyStateChunk MakeWorldTailChunk(uint64_t round, const std::vector<uint8_t>& slice);
	/// The round a tail chunk names; false for a chunk too short to carry one.
	bool ParseWorldTailChunkRound(const NetLobbyStateChunk& chunk, uint64_t& round);
	inline constexpr uint8_t c_NetWorldReportProgress = 1;
	inline constexpr uint8_t c_NetWorldReportCatchUp = 2;
	inline constexpr uint8_t c_NetWorldReportActivate = 3;
	inline constexpr uint8_t c_NetWorldReportRefused = 4;
	inline constexpr uint8_t c_NetWorldReportDecline = 5; //!< A watcher's own choice: 1 declines a seat.
	inline constexpr uint8_t c_NetWorldReportActivationAck = 6;
	inline constexpr uint8_t c_NetWorldReportActivationCommit = 7;
	/// A successor to a returner whose held state predates its handover: the first frame under the new authority.
	inline constexpr uint8_t c_NetWorldReportHandover = 8;
	/// The round ended while the seat was held or rejoining: its end record, the final frame and the winner team + 1 in the top byte.
	inline constexpr uint8_t c_NetWorldReportRoundEnded = 9;
	inline uint64_t PackRoundEndedRecord(uint64_t finalFrame, int winnerTeam) {
		return (finalFrame & 0x00FFFFFFFFFFFFFFULL) | (static_cast<uint64_t>(static_cast<uint8_t>(winnerTeam < 0 ? 0 : winnerTeam + 1)) << 56);
	}
	inline uint64_t RoundEndedFinalFrame(uint64_t record) { return record & 0x00FFFFFFFFFFFFFFULL; }
	/// The winner team, or -1 for a round that ended without one.
	inline int RoundEndedWinnerTeam(uint64_t record) { return static_cast<int>(record >> 56) - 1; }


	/// Why a world turned a connection away, as a code the joiner turns into the line it shows.
	enum class NetWorldJoinRefusal : uint64_t {
		None = 0,
		WorldFull = 1, //!< Every team is at capacity and the spectator bound is spent.
		SeatHeld = 2,  //!< The seat is waiting for its own holder to come back.
	};
	const char* NetWorldJoinRefusalText(uint64_t code);

	/// SHA-256 of the published archive bytes, lowercase hex.
	std::string DigestWorldJoinBytes(const uint8_t* bytes, size_t size);
	/// Packs every required Controller, command, binding and observation from one committed tick.
	NetLockstepFrame PackWorldJoinReadyFrame(const NetLockstepReadyFrame& ready);
	bool EncodeCommittedJoinFrame(const NetLockstepFrame& frame, std::vector<uint8_t>& bytes, std::string* error = nullptr);
	bool DecodeCommittedJoinFrame(const std::vector<uint8_t>& bytes, NetLockstepFrame& frame, std::string* error = nullptr);
	inline std::string DigestWorldJoinBytes(const std::vector<uint8_t>& bytes) {
		return DigestWorldJoinBytes(bytes.data(), bytes.size());
	}

	/// The joiner-only envelope: offer JSON, archive bytes, recovery-encoded tail [B+1, ...].
	bool IsWorldJoinImageBlob(const std::vector<uint8_t>& bytes);
	bool EncodeWorldJoinImageBlob(const NetWorldCheckpointImage& image, const std::vector<uint8_t>& archive,
	                              const std::vector<std::vector<uint8_t>>& tail, std::vector<uint8_t>& out, std::string* error = nullptr);
	bool DecodeWorldJoinImageBlob(const std::vector<uint8_t>& bytes, NetWorldCheckpointImage& image, std::vector<uint8_t>& archive,
	                              std::vector<std::vector<uint8_t>>& tail, std::string* error = nullptr);

	/// A valid one-chunk StateChunk carrying a typed 8-byte value. Empty payloads stay illegal.
	NetLobbyStateChunk MakeWorldJoinReport(uint8_t kind, uint64_t value);
	/// The handover report: the frame as its value, then the generation, the authority and the departed mask.
	NetLobbyStateChunk MakeWorldJoinHandoverReport(const NetWorldHandover& handover);
	/// Reads a handover report's fields as ParseWorldJoinReport returns them.
	NetWorldHandover WorldJoinHandoverFromReport(uint64_t value, uint64_t generation, uint64_t authority, uint64_t departedMask);
	bool ParseWorldJoinReport(const NetLobbyStateChunk& chunk, uint8_t& kind, uint64_t& value, uint64_t* workTicks = nullptr, uint64_t* workUs = nullptr, uint64_t* sentThrough = nullptr);

	/// Host-authored Activate binding: seat, team, brain preset and spawn (Persistent World respawn API).
	NetGameWorldTransition BuildWorldActivateTransition(const NetWorldJoinSession& session, const NetMatchConfig& config, uint64_t membershipRevision);

	/// The activity player slot a world seat owns, or -1 when the roster names no player for it.
	int32_t WorldActivityPlayerOf(const NetMatchConfig& config, uint8_t peerId);

	/// Host-authored respawn of a seated member's brain: the same team spawn the Activate uses,
	/// committed at one frame, so every peer puts the same actor in at the same tick.
	NetGameWorldTransition BuildWorldSeatRespawnTransition(const NetWorldSlot& slot, const NetMatchConfig& config, uint64_t membershipRevision, uint64_t atFrame);

	/// The respawn delay in committed frames. A world that names no delay takes the preset's.
	uint64_t WorldRespawnDelayFrames(const NetMatchConfig& config);

	/// Whether the transition seats a member's own brain: an Activate or its later respawn.
	inline bool WorldTransitionSeatsMember(const NetGameWorldTransition& transition) {
		return transition.kind == NetGameWorldTransition::Activate || transition.kind == NetGameWorldTransition::SeatRespawn;
	}

	/// Whether an applied Activate binds the seat's brain on this peer: the host asked for it, a
	/// resident was seated and the slot is a real player seat.
	bool WorldTransitionBindsBrain(const NetGameWorldTransition& transition, bool seated);

	/// One brain an Activate could seat, read from lockstep state: the committed actor roster's order
	/// and the synced control-handoff map, so every peer offers the same list in the same order.
	struct NetWorldBrainCandidate {
		int64_t actorUID = 0;
		int team = 0;
		uint8_t ownerPeerId = 0; //!< The member holding it through a synced handoff; 0 when none does.
	};

	/// The brain an Activate may seat: the first of the transition's team that no other member holds.
	/// 0 means the activation spawns the transition's own preset instead of taking a resident.
	int64_t ChooseWorldActivateBrain(const std::vector<NetWorldBrainCandidate>& brains, const NetGameWorldTransition& transition);

	/// What a bootstrap whose E has arrived gets: a member is admitted and its Activate is committed;
	/// an overflow spectator only keeps streaming.
	struct NetWorldActivationPlan {
		bool admit = false;
		bool submitTransition = false;
		uint64_t firstRequired = 0;
	};
	NetWorldActivationPlan PlanWorldActivation(const NetWorldJoinSession& session, uint64_t nextFrame, bool late);

	/// The host's join plane: the slot table, the image in flight, the tail, the per-connection
	/// bootstraps and the measurements. It authors transitions; it never polls a transport.
	class NetWorldJoinHost {
	public:
		bool Configure(const NetMatchConfig& config, const NetWorldIdentity& identity, std::string* error = nullptr);
		bool ConfigureMatchRejoins(const NetMatchConfig& config, uint64_t roundId, double tickMs, std::string* error = nullptr);
		bool BeginRejoin(NetPeerId connection, uint16_t stableSeat, uint8_t peerId, uint32_t incarnation, const std::string& name, uint64_t nowMs, std::string* error = nullptr);
		/// A held seat whose player kept its state: its catch-up streams the committed tail from the tick that state stands at, with no image.
		bool BeginInPlaceRejoin(NetPeerId connection, uint16_t stableSeat, uint8_t peerId, uint32_t incarnation, const std::string& name, uint64_t nowMs, uint64_t heldThrough, std::string* error = nullptr);
		bool NoteRejoinCapacity(NetPeerId connection, uint64_t workTicks, uint64_t workUs, uint64_t sentThrough);
		void NoteRejoinLinkFit(NetPeerId connection, bool fits);
		/// The session whose catch-up gate is due in the log (a change, or a second of the round since); gate, when set, is the one its report met first.
		const NetWorldJoinSession* TakeCatchUpGateToLog(NetPeerId connection, const char* gate, uint64_t nowFrame);
		bool IsConfigured() const { return m_Identity.IsValid() || m_PrivateRound != 0; }
		bool IsPrivateMatch() const { return m_PrivateRound != 0; }
		const NetWorldIdentity& Identity() const { return m_Identity; }

		/// How many live bootstraps are watching rather than holding a slot.
		size_t SpectatorCount() const;
		/// The spectator bound this world was configured with, never wider than the lobby-id pool.
		size_t SpectatorBound() const;
		/// Watchers the world could still admit right now; what the directory row advertises.
		size_t SpectatorsFree() const;
		/// Records that a connection was turned away, so the world answers it once instead of
		/// reopening the same refusal every pump. Returns whether this call was the first.
		bool NoteRefusal(NetPeerId connection, NetWorldJoinRefusal refusal);
		/// The refusal a connection already carries; None when it was never turned away.
		NetWorldJoinRefusal RefusalOf(NetPeerId connection) const;
		/// Opens a bootstrap for an authenticated connection. Refuses a second one for the same
		/// connection rather than opening a parallel transfer.
		/// @param nowMs The host's admission clock, so a stalled transfer can expire.
		/// @param credentialedHolder Whether the admission plane says this connection is the seat's
		/// own returning holder. Only it may take back a slot a reclaim hold is keeping.
		/// @param seatPeerId The lockstep id of the seat the roster admitted this connection to; its slot is taken when free.
		bool BeginJoin(NetPeerId connection, uint16_t stableSeat, const std::string& holderName, uint64_t nowMs, std::string* error = nullptr, bool credentialedHolder = false,
		               uint8_t seatPeerId = 0);
		/// Records which slots are waiting for a dropped holder, from the admission plane's seats.
		void NoteReclaimHolds(const std::vector<uint8_t>& peerIds);
		/// A watcher's own choice: a spectator that declines is skipped when a slot frees.
		bool NoteSpectatorPreference(NetPeerId connection, bool declinesPromotion);
		/// Gives a freed slot to the oldest watcher that wants it, through the same announced
		/// activation a fresh join takes: one promotion per call, one E, one brain.
		/// @param outConnection The promoted watcher; unchanged when none was.
		/// @param bind Records the watcher's seat on the slot before the world seats it; nothing changes when it refuses.
		bool PromoteWaitingSpectator(uint64_t nowFrame, uint64_t* outActivationTick, NetPeerId* outConnection, std::string* error = nullptr,
		                             const std::function<bool(const NetWorldJoinSession&, const NetWorldSlot&)>& bind = {});
		/// Binds the frozen image to every bootstrap still waiting for one.
		void PublishImage(const NetWorldCheckpointImage& image);
		const NetWorldCheckpointImage& Image() const { return m_Image; }
		/// Whether the round's history, on disk or in memory, still holds every frame after the published image, so a joiner sent it
		/// can catch up from it; false without an image or a history.
		bool ImageHistoryServable() const { return m_Image.IsValid() && m_Tail.Count() != 0 && m_Image.tick + 1 >= m_Tail.FirstServableFrame(); }
		/// Whether the round's history has moved past the frame after the published image, so no joiner can catch up from it.
		bool ImageHistoryLost() const { return m_Image.IsValid() && m_Tail.Count() != 0 && m_Image.tick + 1 < m_Tail.FirstServableFrame(); }
		/// Records that the joiner has the whole image and has begun replaying the tail.
		bool NoteTransferComplete(NetPeerId connection, uint64_t bytes, std::string* error = nullptr);
		bool NoteTransferProgress(NetPeerId connection, uint16_t ackedChunks, uint16_t totalChunks);
		bool NoteDeliveredThrough(NetPeerId connection, uint64_t frame);
		bool NextTailChunk(NetPeerId connection, std::vector<uint8_t>& chunk);
		/// The next tail datagram due to a catching-up connection, packed as whole frames: the lowest one its replay has not passed
		/// within its resend time, or the frames after the last one sent. Returns false when none is due.
		/// large: the next frame is too large for a datagram, or a batch of them is on its way in pieces (NextTailChunk carries it).
		bool NextTailDatagram(NetPeerId connection, uint64_t nowMs, std::vector<uint8_t>& packed, bool* large = nullptr);
		/// Retires the datagrams a connection's replay has passed and measures their round trip.
		void AcknowledgeTailDatagrams(NetPeerId connection, uint64_t nowMs);
		/// Counts a tail datagram the transport would not take.
		void NoteTailDatagramRefused(NetPeerId connection);
		/// Notes the link's own round trip to a returner: the tail's resends are timed on it, not on a replay that may trail its arrivals.
		void NoteTailLinkRtt(NetPeerId connection, uint32_t rttMs);
		void NoteTailChunkSent(NetPeerId connection, size_t bytes);
		bool NoteTransferStarted(NetPeerId connection, uint64_t transferId, uint16_t totalChunks, uint64_t deliveredThrough);
		/// Records that this bootstrap has been sent the match config, so a retried transfer does not
		/// send it again on every pump. Returns whether this call was the first.
		bool NoteMatchConfigSent(NetPeerId connection);
		/// Records the tail the joiner has applied and, once it has caught the world, schedules E.
		/// @param nowFrame The world's committed frame.
		/// @param outActivationTick The announced activation tick when this call scheduled one.
		bool NoteCatchUpProgress(NetPeerId connection, uint64_t appliedThrough, uint64_t ticksReplayed, uint64_t elapsedMs, uint64_t nowFrame, uint64_t* outActivationTick, std::string* error = nullptr);
		void NoteCatchUpClock(NetPeerId connection, uint64_t nowMs);
		/// Returning seats that have replayed for longer than the bound without coming inside the activation lead.
		/// A returner that replays slower than the round plays never closes on it, so it is never activated.
		std::vector<NetPeerId> ReturnersWithoutHeadroom(uint64_t nowMs, uint64_t boundMs) const;
		/// Whether a returner's replay stands inside the activation lead of the round's committed horizon.
		static bool ShowsReplayHeadroom(const NetWorldJoinSession& session);
		/// The bootstrap whose activation tick has arrived and whose joiner has applied through E-1.
		const NetWorldJoinSession* DueActivation(uint64_t nowFrame) const;
		/// Applied through E-1 after the E-1 pump, so Admit uses max(E, nextFrame + 1).
		const NetWorldJoinSession* LateActivation(uint64_t nowFrame) const;
		/// A catching-up joiner that missed E and still sits behind it.
		const NetWorldJoinSession* SlowActivation(uint64_t nowFrame) const;
		/// The highest target the round has already put on the wire. Every activation is announced
		/// ahead of it, so a member is a peer of the round before the frames it owes go out.
		void NoteSentInputThrough(uint64_t lastQueuedTarget) { m_SentInputThrough = lastQueuedTarget; }
		uint64_t SentInputThrough() const { return m_SentInputThrough; }
		/// Announces a later E once. A second miss is a CancelJoin.
		bool ReannounceActivation(NetPeerId connection, uint64_t nowFrame, uint64_t* outActivationTick, std::string* error = nullptr);
		/// Marks the bootstrap active once its transition has been committed.
		bool CompleteActivation(NetPeerId connection, uint64_t atFrame, std::string* error = nullptr);
		void AcknowledgeActivation(NetPeerId connection, uint64_t frame);
		void MarkActivationProposed(NetPeerId connection);
		void MarkActivationCommitted(NetPeerId connection);
		/// Ends a bootstrap without a seat drop: a failed or slow fresh join is not a departure.
		void CancelJoin(NetPeerId connection, const std::string& reason);
		/// Ends a bootstrap the round's history can no longer serve. A seat its player is coming back to stays held for that player, as a
		/// dropped link leaves it; a fresh join gives its slot back as CancelJoin does, and a watcher its lobby id.
		void EndBootstrap(NetPeerId connection, const std::string& reason);
		/// Whether a reader has trailed the round past the lag limit for a whole limit of round time without gaining on it. A reader that
		/// gains is closing, however far behind, and its window starts again.
		bool TrailsPastLagLimit(NetPeerId connection, uint64_t tick, uint64_t limitFrames);
		/// The peers whose joins were cancelled since the last call, oldest first.
		std::vector<uint8_t> TakeCancelledJoins();
		/// Cancels every bootstrap past its deadline. Returns how many it ended.
		size_t ExpireStaleJoins(uint64_t nowMs, std::vector<NetPeerId>* expired = nullptr);
		/// Ends every bootstrap whose connection is gone, so a spectator's lobby id returns to the pool.
		/// Returns how many it ended.
		size_t ReleaseLostConnections(const std::vector<NetPeerId>& liveConnections);

		const std::vector<NetWorldJoinSession>& Sessions() const { return m_Sessions; }
		bool HasBootstrapInFlight() const;
		bool HasImageTransferInFlight() const;
		const NetWorldJoinSession* FindSession(NetPeerId connection) const;
		bool BeginFinalTail(NetPeerId connection, uint64_t finalFrame);
		NetWorldMembership& Membership() { return m_Membership; }
		const NetWorldMembership& Membership() const { return m_Membership; }
		NetWorldFrameLog& Tail() { return m_Tail; }
		const NetWorldFrameLog& Tail() const { return m_Tail; }
		/// What its joins, history and image hold, as counts and bytes, for the memory census.
		std::string MemoryCensus() const;
		/// The round a joiner's catch-up names for this tail: the match round of a rejoin plane, the boot round of a world.
		uint64_t TailRound() const { return m_PrivateRound != 0 ? m_PrivateRound : m_Identity.round; }
		NetWorldMetrics& Metrics() { return m_Metrics; }
		const NetWorldMetrics& Metrics() const { return m_Metrics; }
		/// The oldest tail frame any live bootstrap still needs; 0 when none does.
		uint64_t OldestNeededFrame() const;
		std::string BuildReportJson() const;
		void Reset();
		/// The record path this host advanced before it listened; empty off a world.
		const std::string& IdentityPath() const { return m_IdentityPath; }
		void SetIdentityPath(const std::string& path) { m_IdentityPath = path; }

	private:
		NetWorldJoinSession* Find(NetPeerId connection);
		/// E: the lead ahead of the committed frame, never at or behind the input already sent.
		uint64_t ChooseActivationTick(uint64_t nowFrame) const;
		uint8_t AllocateSpectatorLobbyPeer() const;
		std::string m_IdentityPath;

		NetWorldIdentity m_Identity;
		uint64_t m_PrivateRound = 0;
		double m_SimTickMs = 0;
		NetMatchConfig m_Config;
		NetWorldMembership m_Membership;
		NetWorldFrameLog m_Tail;
		NetWorldMetrics m_Metrics;
		NetWorldCheckpointImage m_Image;
		std::vector<NetWorldJoinSession> m_Sessions;
		std::vector<std::pair<NetPeerId, NetWorldJoinRefusal>> m_Refused; //!< Connections already turned away.
		uint64_t m_NextJoinOrder = 1;    //!< Stamped on every bootstrap, so promotion reads join order.
		uint64_t m_Promotions = 0;
		uint64_t m_SentInputThrough = 0; //!< The round's highest sent target, from the coordinator.
		uint64_t m_ActivationsCommitted = 0;
		uint64_t m_JoinsCancelled = 0;
		std::vector<uint8_t> m_CancelledJoins; //!< The assigned peers of the joins cancelled, for the host's seat roster.
	};

} // namespace RTE
