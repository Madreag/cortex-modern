#pragma once

#include "NetLobbyProtocol.h"
#include "NetLockstep.h"
#include "NetMatchConfig.h"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
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

		bool IsValid() const { return NetMatchConfigUtil::IsWorldId(worldId) && boot != 0; }
		bool operator==(const NetWorldIdentity&) const = default;
	};

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

		bool IsValid() const { return NetMatchConfigUtil::IsWorldId(worldId) && boot != 0 && tick != 0 && bytes != 0; }
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
		uint32_t holderGeneration = 0;
		uint8_t assignedPeerId = 0;       //!< The lockstep id activation will install; 0 until the host picks one.
		int8_t team = -1;
		bool spectator = false;
		uint64_t snapshotTick = 0;        //!< B.
		uint64_t activationTick = 0;      //!< E, announced before it arrives; 0 until scheduled.
		uint64_t deliveredThrough = 0;    //!< The last tail frame this connection has been sent.
		uint64_t acknowledgedThrough = 0; //!< The last tail frame it says it applied.
		uint64_t openedAtMs = 0;
		uint64_t transferBytes = 0;
		uint64_t transferId = 0;          //!< The StateChunk transfer the joiner is receiving.
		uint16_t ackedChunks = 0;
		uint16_t totalChunks = 0;
		uint32_t activationReannounces = 0; //!< At most one later E; then the slot is freed.
		bool transferStarted = false;
		uint64_t catchUpTicks = 0;        //!< Ticks it reported replaying, for the catch-up rate.
		uint64_t catchUpMs = 0;
		uint64_t lastCatchUpReportMs = 0; //!< Host clock of the last catch-up report, for elapsed.
		uint8_t spectatorLobbyPeer = 0;   //!< Non-member lobby id in [32, 47]; 0 if none remains.
		std::string refusal;              //!< Why the bootstrap failed; empty while it is alive.
	};

	/// The bounded log of canonical committed frames from B+1 onward. A joiner applies these at the
	/// fixed timestep after restoring the image, so the world never waits for it.
	class NetWorldFrameLog {
	public:
		static constexpr size_t c_DefaultMaxFrames = 3600;              //!< A minute of 60 Hz ticks.
		static constexpr uint64_t c_DefaultMaxBytes = 32ULL * 1024 * 1024;

		void Configure(size_t maxFrames, uint64_t maxBytes);
		/// Encodes and retains one committed frame. Frames must arrive in order and without gaps.
		bool Append(const NetLockstepFrame& frame, std::string* error = nullptr);
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
		void Clear();

	private:
		struct Record {
			uint64_t frame = 0;
			std::vector<uint8_t> bytes;
		};

		void Trim();

		std::deque<Record> m_Records;
		size_t m_MaxFrames = c_DefaultMaxFrames;
		uint64_t m_MaxBytes = c_DefaultMaxBytes;
		uint64_t m_Bytes = 0;
		uint64_t m_Evicted = 0;
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
		bool held = false;
		std::string holderName;
	};

	/// The host's slot table. The order is configured, never derived from a live peer count: a clean
	/// leave frees its slot under a new generation, and a credentialed reclaim outranks a fresh join.
	class NetWorldMembership {
	public:
		/// Builds the fixed order from the world's config: one slot per non-host peer id, teams in the
		/// config's team order. Called once at boot and after a restart, never mid-round.
		bool Configure(const NetMatchConfig& config, std::string* error = nullptr);
		/// The slot a fresh join takes: the first free one in the configured order. Null when the
		/// world is full, which makes the joiner a spectator rather than a refusal.
		const NetWorldSlot* FirstFreeSlot() const;
		/// The slot a credentialed holder reclaims; null when the seat is not this world's.
		const NetWorldSlot* SlotOfSeat(uint16_t stableSeat) const;
		bool Hold(uint8_t peerId, uint16_t stableSeat, const std::string& holderName, std::string* error = nullptr);
		/// Frees the slot and advances its generation. A later return of the same player is a fresh
		/// admission: the world never silently promises back the character it had.
		bool Release(uint8_t peerId, std::string* error = nullptr);
		const std::vector<NetWorldSlot>& Slots() const { return m_Slots; }
		size_t FreeSlots() const;
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
	/// World-join plane schema on the offer, the transition and the membership report.
	inline constexpr uint16_t c_NetWorldJoinSchema = 1;
	/// Overflow spectators bind lobby ids in [first, last], one per connection, above member seats.
	inline constexpr uint8_t c_WorldSpectatorLobbyPeerFirst = 32;
	inline constexpr uint8_t c_WorldSpectatorLobbyPeerLast = 47;
	inline constexpr size_t c_WorldSpectatorLobbyCap = static_cast<size_t>(c_WorldSpectatorLobbyPeerLast - c_WorldSpectatorLobbyPeerFirst + 1);

	inline uint8_t WorldJoinLobbyPeer(const NetWorldJoinSession& session) {
		if (session.assignedPeerId != 0) {
			return session.assignedPeerId;
		}
		return session.spectatorLobbyPeer;
	}

	/// The joiner's own bootstrap state: the image it restored, the tail it holds and the E it was given.
	struct NetWorldCatchUpClient {
		bool active = false;
		uint64_t snapshotTick = 0;        //!< B, the tick the restored image froze at.
		uint64_t appliedThrough = 0;      //!< The last committed tail frame the sim has applied.
		uint64_t activationTick = 0;      //!< E, once the host has announced it.
		std::string digest;
		std::vector<NetLockstepFrame> tail;
	};

	/// The catch-up report a joiner sends: what its sim has applied, never the host's frame.
	NetLobbyStateChunk MakeJoinerCatchUpReport();
	/// WJIM: the joiner-only checkpoint envelope streamed through the lobby StateChunk pump.
	inline constexpr uint32_t c_NetWorldImageMagic = 0x4D494A57U;
	inline constexpr uint8_t c_NetWorldImageVersion = 1;
	/// A valid 9-byte StateChunk the joiner and host exchange for progress, catch-up and E.
	inline constexpr uint64_t c_NetWorldReportTransferId = 0x574A5250ULL;
	inline constexpr uint64_t c_NetWorldTailTransferId = 0x5441494CULL;
	inline constexpr uint8_t c_NetWorldReportProgress = 1;
	inline constexpr uint8_t c_NetWorldReportCatchUp = 2;
	inline constexpr uint8_t c_NetWorldReportActivate = 3;

	/// SHA-256 of the published archive bytes, lowercase hex.
	std::string DigestWorldJoinBytes(const uint8_t* bytes, size_t size);
	/// Packs every required Controller, command, binding and observation from one committed tick.
	NetLockstepFrame PackWorldJoinReadyFrame(const NetLockstepReadyFrame& ready);
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
	bool ParseWorldJoinReport(const NetLobbyStateChunk& chunk, uint8_t& kind, uint64_t& value);

	/// Host-authored Activate binding: seat, team, brain preset and spawn (Persistent World respawn API).
	NetGameWorldTransition BuildWorldActivateTransition(const NetWorldJoinSession& session, const NetMatchConfig& config, uint64_t membershipRevision);

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
		bool IsConfigured() const { return m_Identity.IsValid(); }
		const NetWorldIdentity& Identity() const { return m_Identity; }

		/// Opens a bootstrap for an authenticated connection. Refuses a second one for the same
		/// connection rather than opening a parallel transfer.
		/// @param nowMs The host's admission clock, so a stalled transfer can expire.
		bool BeginJoin(NetPeerId connection, uint16_t stableSeat, const std::string& holderName, uint64_t nowMs, std::string* error = nullptr);
		/// Binds the frozen image to every bootstrap still waiting for one.
		void PublishImage(const NetWorldCheckpointImage& image);
		const NetWorldCheckpointImage& Image() const { return m_Image; }
		/// Records that the joiner has the whole image and has begun replaying the tail.
		bool NoteTransferComplete(NetPeerId connection, uint64_t bytes, std::string* error = nullptr);
		bool NoteTransferProgress(NetPeerId connection, uint16_t ackedChunks, uint16_t totalChunks);
		bool NoteDeliveredThrough(NetPeerId connection, uint64_t frame);
		bool NoteTransferStarted(NetPeerId connection, uint64_t transferId, uint16_t totalChunks, uint64_t deliveredThrough);
		/// Records the tail the joiner has applied and, once it has caught the world, schedules E.
		/// @param nowFrame The world's committed frame.
		/// @param outActivationTick The announced activation tick when this call scheduled one.
		bool NoteCatchUpProgress(NetPeerId connection, uint64_t appliedThrough, uint64_t ticksReplayed, uint64_t elapsedMs, uint64_t nowFrame, uint64_t* outActivationTick, std::string* error = nullptr);
		void NoteCatchUpClock(NetPeerId connection, uint64_t nowMs);
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
		/// Announces E for an overflow spectator (no Controller, no Admit).
		bool ScheduleSpectatorActivation(NetPeerId connection, uint64_t nowFrame, uint64_t* outActivationTick, std::string* error = nullptr);
		/// Announces a later E once. A second miss is a CancelJoin.
		bool ReannounceActivation(NetPeerId connection, uint64_t nowFrame, uint64_t* outActivationTick, std::string* error = nullptr);
		/// Marks the bootstrap active once its transition has been committed.
		bool CompleteActivation(NetPeerId connection, uint64_t atFrame, std::string* error = nullptr);
		/// Ends a bootstrap without a seat drop: a failed or slow fresh join is not a departure.
		void CancelJoin(NetPeerId connection, const std::string& reason);
		/// Cancels every bootstrap past its deadline. Returns how many it ended.
		size_t ExpireStaleJoins(uint64_t nowMs);
		/// Ends every bootstrap whose connection is gone, so a spectator's lobby id returns to the pool.
		/// Returns how many it ended.
		size_t ReleaseLostConnections(const std::vector<NetPeerId>& liveConnections);

		const std::vector<NetWorldJoinSession>& Sessions() const { return m_Sessions; }
		const NetWorldJoinSession* FindSession(NetPeerId connection) const;
		NetWorldMembership& Membership() { return m_Membership; }
		const NetWorldMembership& Membership() const { return m_Membership; }
		NetWorldFrameLog& Tail() { return m_Tail; }
		const NetWorldFrameLog& Tail() const { return m_Tail; }
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
		NetMatchConfig m_Config;
		NetWorldMembership m_Membership;
		NetWorldFrameLog m_Tail;
		NetWorldMetrics m_Metrics;
		NetWorldCheckpointImage m_Image;
		std::vector<NetWorldJoinSession> m_Sessions;
		uint64_t m_SentInputThrough = 0; //!< The round's highest sent target, from the coordinator.
		uint64_t m_ActivationsCommitted = 0;
		uint64_t m_JoinsCancelled = 0;
	};

} // namespace RTE
