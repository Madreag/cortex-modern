#pragma once

#include "ControllerFrame.h"
#include "NetGameCommand.h"
#include "NetMatchConfig.h"
#include "NetTransport.h"
#include <algorithm>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <utility>
#include <vector>

namespace RTE {
	class Actor;
	struct NetResyncPendingCommand;

	/// The live match's lockstep clock: monotonic milliseconds every caller that drives a coordinator
	/// reads, so the missing-frame grace is wall time and never steps back between setup and play.
	/// Selftests inject their own values instead.
	uint64_t NetLockstepNowMs();

	enum class NetLockstepPacketType : uint16_t {
		Start = 1,
		Frame = 2,
		Ack = 3,
		Stop = 4,
		Checksum = 5,
		SeatSnapshot = 6,
		RecoveryChunk = 7,
		Timing = 8,
	};

	enum class NetLockstepStopReason : uint16_t {
		Complete = 1,
		MissingFrameTimeout = 2,
		Desync = 3,
		ProtocolError = 4,
		PeerDisconnected = 5,
		InternalError = 6,
		PeerLeft = 7, // A clean leave: the frame field is the FIRST frame without the leaver's data; survivors continue.
		PeerDropped = 9, // The same, for a transport that died: the seat may still be reclaimed, so survivors hold a scripted outcome until the reclaim frame.
		ResyncRequested = 8, // The host ends the round so everyone reconvenes and reloads its snapshot (rejoin/heal).
		Reclaimed = 10, // Host: the held seat's player was admitted back; resume through the rejoin path at this frame.
		Substituted = 11, // Host: a moderator reseat takes the held seat; resume through the reseat path at this frame.
		PeerRemoved = 13,
		Expired = 12, // Host: the admission hold timed out; the seat is gone and commits resume without it.
	};

	enum class NetLockstepHoldResolution : uint16_t {
		None = 0,
		Reclaimed = 1,
		Substituted = 2,
		Expired = 3,
	};

	enum class NetSeatPresenceState : uint8_t {
		Present = 0,
		Disconnected = 1,
		Reconnecting = 2,
		Substituted = 3,
		Left = 4,
	};

	/// Public admission state. It describes a seat and never grants simulation authority.
	struct NetSeatPresenceEntry {
		uint16_t stableSeat = 0;
		uint8_t peerId = 0;
		NetSeatPresenceState state = NetSeatPresenceState::Present;
		uint32_t holderGeneration = 0;
		uint32_t seatGeneration = 0;
		uint32_t incarnation = 0;
		bool holdActive = false;
		uint64_t holdUntilMs = 0; //!< Host admission-clock deadline, independent of the simulation hold.
		uint64_t holdUntilFrame = 0;
		std::string holderName;

		bool operator==(const NetSeatPresenceEntry&) const = default;
	};

	/// A full roster update, authored by the host rather than by any of its subject seats.
	struct NetLockstepSeatSnapshot {
		uint8_t senderPeerId = 0;
		uint64_t sessionId = 0;
		std::array<uint8_t, 16> epoch{};
		uint64_t roundId = 0;
		uint64_t revision = 0;
		uint64_t observedAtMs = 0;
		std::vector<NetSeatPresenceEntry> seats;

		bool operator==(const NetLockstepSeatSnapshot&) const = default;
	};

	enum class NetLockstepErrorCode {
		None,
		NullBuffer,
		ShortHeader,
		BadMagic,
		UnsupportedVersion,
		BadHeaderSize,
		UnknownFlags,
		UnknownPacketType,
		ReservedFieldNonZero,
		PayloadLengthMismatch,
		PayloadTooLarge,
		TruncatedPayload,
		TrailingBytes,
		StringTooLong,
		InvalidString,
		InvalidValue,
		EncodeFailed,
		UnboundObservationSlot, //!< An observation named a slot this sender never spelled out.
		ObservationBindingGap, //!< A sender's bindings do not follow on from the ones this peer has.
	};

	struct NetLockstepError {
		NetLockstepErrorCode code = NetLockstepErrorCode::None;
		size_t offset = 0;
		std::string message;
	};

	struct NetLockstepStart {
		uint64_t sessionId = 0;
		uint64_t startFrame = 0;
		uint16_t inputDelayFrames = 0;
		uint16_t controllerFrameVersion = 0;
		uint16_t controllerFrameEncodedSize = 0;
		uint8_t localPeerId = 0;
		uint8_t peerCount = 0;
		std::string scenario;
		std::string ownershipPolicy;
		uint64_t roundId = 0; //!< The host's tag for this lockstep round; a client adopts it from the host's start.
		bool resumeFromSnapshot = false;
		uint32_t activityRestartMs = 0; //!< This peer's own measured activity restart; 0 until it has one.
		bool startupPublished = false; //!< Whether that measurement is a reading and not an absence.
		uint8_t deviceClass = 0; //!< The sender's seat device (Controller::WireDeviceClass); 0 until it has sampled one.
		// A host-authored start boundary. Ordinary starts carry one peer's publication; this record is
		// the single fact every peer applies, including the host that authored it.
		bool agreedStartRecord = false;
		uint64_t agreedFirstFrame = 0;
		uint64_t agreedEffectiveStartFrame = 0;
		uint64_t agreedDeadlineMs = 0;
		uint32_t publishedPeerMask = 0;
		uint32_t heldPeerMask = 0;
		std::array<uint64_t, 16> peerEffectiveStartFrames{};
		std::array<uint32_t, 16> peerStartupParks{};
		std::array<uint16_t, 16> peerInputDelays{};
		std::array<uint8_t, 16> peerDeviceClasses{}; //!< Each seat's device as its peer published it; what a script reads before the seat's first frame.

		bool operator==(const NetLockstepStart&) const = default;
	};

	/// One peer's actual audibility of a shared simulation sound, sampled at its input boundary and
	/// committed with the sender's delayed frame so every peer reads the identical value.
	struct NetSoundObservation {
		uint8_t senderPeerId = 0;
		uint64_t objectUID = 0;
		uint64_t tick = 0;
		uint64_t phase = 0;
		uint64_t occurrence = 0;
		uint64_t ordinal = 0;
		float value = 0.0F;

		bool operator==(const NetSoundObservation&) const = default;
	};

	/// One peer's local-AI write to a MovableObject number or string map, sampled at its input
	/// boundary and committed with the sender's delayed frame.
	struct NetValueObservation {
		uint8_t senderPeerId = 0;
		uint64_t objectUID = 0;
		uint64_t tick = 0;
		uint32_t ordinal = 0;
		uint8_t mapKind = 0;
		std::string key;
		uint8_t op = 0;
		double numberValue = 0;
		std::string stringValue;

		bool operator==(const NetValueObservation&) const = default;
	};

	/// Which sound an observation is about, without the reading. The wire spells one of these out once
	/// per sender and refers to it by slot afterwards.
	struct NetSoundObservationKey {
		uint64_t objectUID = 0;
		uint64_t tick = 0;
		uint64_t phase = 0;
		uint64_t occurrence = 0;
		uint64_t ordinal = 0;

		auto operator<=>(const NetSoundObservationKey&) const = default;
	};

	NetSoundObservationKey KeyOfObservation(const NetSoundObservation& observation);

	/// One sender's slot table for the compact observation form, held for the length of a lockstep
	/// round. A key rides the wire the first time that sender sends it and is a slot number after
	/// that, so a stream of changing readings costs five bytes each instead of forty-four. Every
	/// binding is explicit in the packet, so the receiver needs no eviction rule of its own and a
	/// repeated packet rebinds the same slot to the same key.
	class NetSoundObservationDictionary {
	public:
		static constexpr uint16_t c_MaxSlots = 4096;

		/// Encoder: which slot spells this key, and whether the key itself must ride along.
		void Assign(const NetSoundObservationKey& key, uint16_t& outSlot, bool& outFullKey);
		/// Encoder: what Assign would cost, without spending a slot on an observation that will not fit.
		bool Lookup(const NetSoundObservationKey& key, uint16_t& outSlot) const;
		/// Decoder: adopt what the sender just spelled out.
		void Bind(uint16_t slot, const NetSoundObservationKey& key);
		bool Resolve(uint16_t slot, NetSoundObservationKey& outKey) const;
		void Reset();
		/// How many keys this sender has spelled out. A packet says what this was before its own
		/// bindings, so a receiver that missed one refuses rather than reading a reused slot as the key
		/// it held before.
		uint64_t BindingCount() const { return m_Bindings; }
		/// Decoder: the newest tick of a round whose block this table has read. A sender's next round starts it over.
		void NoteTickRead(uint64_t tick, uint64_t round) {
			if (!m_HasReadTick || round != m_LastTickRound || tick > m_LastTickRead) m_LastTickRead = tick;
			m_LastTickRound = round;
			m_HasReadTick = true;
		}
		bool HasReadTickAtOrAfter(uint64_t tick, uint64_t round) const { return m_HasReadTick && round == m_LastTickRound && m_LastTickRead >= tick; }

	private:
		struct Slot {
			NetSoundObservationKey key;
			bool bound = false;
			std::list<uint16_t>::iterator recency{};
		};

		std::vector<Slot> m_Slots;
		std::map<NetSoundObservationKey, uint16_t> m_SlotOf;
		std::list<uint16_t> m_Recent; //!< Least recently assigned first: the slot to reuse once every slot is bound.
		uint64_t m_Bindings = 0;
		uint64_t m_LastTickRead = 0;
		uint64_t m_LastTickRound = 0;
		bool m_HasReadTick = false;
	};

	/// The observation tables one peer keeps for the senders it decodes and, on the relay host,
	/// re-encodes. A round's tables are reset when the round starts.
	struct NetSoundObservationTables {
		std::map<uint8_t, NetSoundObservationDictionary> bySender;
		/// The round these tables belong to. A frame from any other round is read past without touching
		/// them, so a packet the round is going to discard can never disturb a live sender's slots.
		uint64_t roundId = 0;
		/// Relay host: the lockstep peer this transport actually is, so a frame claiming another
		/// sender can only ever disturb its own table. Zero on a client, whose one link is the host.
		uint8_t transportSender = 0;

		NetSoundObservationDictionary& For(uint8_t senderPeerId) { return bySender[transportSender != 0 ? transportSender : senderPeerId]; }
		/// The table of a named sender, whatever transport is being decoded: what this peer encodes its
		/// own frames with, and what a relay host re-encodes another peer's frames with.
		NetSoundObservationDictionary& Exactly(uint8_t senderPeerId) { return bySender[senderPeerId]; }
		void Reset() { bySender.clear(); roundId = 0; }
	};

	struct NetLockstepFrame {
		uint8_t senderPeerId = 0;
		uint64_t targetFrame = 0;
		std::vector<ControllerFrame> frames;
		std::vector<NetGameCommand> commands;
		uint64_t roundId = 0;
		std::vector<NetSoundObservation> observations;
		std::vector<NetValueObservation> valueObservations;
		/// Older ticks riding this packet, oldest first. Empty on the classic reserved=0 path.
		std::vector<NetLockstepFrame> priorWindow;
		/// Decode state, never a wire field: this copy's observations stood behind the reader's table, so
		/// they were read past and the tick is already in this peer's stream.
		bool observationsReadPast = false;

		bool operator==(const NetLockstepFrame& rhs) const;
	};

	/// The observation bytes one tick went out with. A window repeat sends these again exactly, so the
	/// slots and the binding count a repaired tick carries are the ones its first send wrote.
	struct NetLockstepObservationBlock {
		std::vector<uint8_t> soundBytes;
		std::vector<uint8_t> valueBytes; //!< Empty when the tick encoded no value observation.
		size_t observationsEncoded = 0;
		size_t valueObservationsEncoded = 0;
	};

	/// Per sender, the blocks of the ticks still inside the redundancy window, by target frame.
	using NetLockstepObservationBlocks = std::map<uint64_t, NetLockstepObservationBlock>;

	struct NetLockstepAck {
		uint8_t senderPeerId = 0;
		uint64_t highestContiguousFrame = 0;
		uint32_t receivedMask = 0;
		uint64_t roundId = 0;
		uint32_t seatIncarnation = 0;
		uint64_t sessionId = 0;
		uint64_t authorityGeneration = 0;

		bool operator==(const NetLockstepAck&) const = default;
	};

	enum class NetTimingAction : uint8_t { Delay = 1, Hold = 2, Reclaim = 3, WorldAdmission = 4, CapturePark = 5 };
	enum class NetTimingPhase : uint8_t { Propose = 1, Acknowledge = 2, Commit = 3, Status = 4, HoldAtFrame = 5, HoldAppliedAck = 6, ReclaimAtFrame = 7 };

	/// A round-scoped delay agreement or host-authored hold and its application acknowledgement.
	struct NetLockstepTiming {
		uint8_t senderPeerId = 0;
		uint8_t peerId = 0;
		NetTimingAction action = NetTimingAction::Delay;
		NetTimingPhase phase = NetTimingPhase::Propose;
		uint64_t sessionId = 0;
		uint64_t roundId = 0;
		uint64_t revision = 0;
		uint64_t applyFrame = 0;
		uint64_t nextFrame = 0;
		uint16_t delayFrames = 0;
		uint8_t requiredPeers = 0;
		uint8_t heldPeers = 0;
		uint32_t pingMs = 0;
		uint32_t jitterMs = 0;
		uint64_t authorityGeneration = 0;
		uint64_t cutoffFrame = 0;
		uint64_t neutralThroughFrame = 0;
		/// The proposal this decision replaces; every peer drops that revision when it applies this one.
		uint64_t supersededRevision = 0;
		std::array<uint32_t, 4> seatIncarnations{};
		std::optional<NetGameWorldTransition> worldTransition;
		bool operator==(const NetLockstepTiming&) const = default;
	};

	struct NetLockstepRecoveryChunk {
		uint8_t senderPeerId = 0;
		uint64_t sessionId = 0;
		uint64_t roundId = 0;
		uint64_t targetFrame = 0;
		uint32_t totalBytes = 0;
		uint32_t offset = 0;
		std::vector<uint8_t> bytes;

		bool operator==(const NetLockstepRecoveryChunk&) const = default;
	};

	struct NetLockstepStop {
		uint8_t senderPeerId = 0;
		NetLockstepStopReason reason = NetLockstepStopReason::InternalError;
		uint64_t frame = 0;
		std::string message;

		bool operator==(const NetLockstepStop&) const = default;
	};

	// A sim-gated state hash for one tick, exchanged periodically so the peers detect a silent divergence.
	struct NetLockstepChecksum {
		uint8_t senderPeerId = 0;
		uint64_t frame = 0;
		std::array<uint8_t, 32> hash{};
		uint64_t roundId = 0;
		std::map<uint8_t, uint64_t> appliedCommands;

		bool operator==(const NetLockstepChecksum&) const = default;
	};

	using NetLockstepPayload = std::variant<NetLockstepStart, NetLockstepFrame, NetLockstepAck, NetLockstepStop, NetLockstepChecksum, NetLockstepSeatSnapshot, NetLockstepRecoveryChunk, NetLockstepTiming>;

	struct NetLockstepPacket {
		NetLockstepPayload payload;

		bool operator==(const NetLockstepPacket&) const = default;
	};

	struct NetLockstepDecodeResult {
		bool ok = false;
		NetLockstepPacket packet;
		NetLockstepError error;
	};

	enum class NetLockstepState {
		Idle,
		WaitingForStart,
		Running,
		Stopped,
		Failed,
	};

	struct NetLockstepConfig {
		uint64_t sessionId = 0;
		uint64_t startFrame = 0;
		uint16_t inputDelayFrames = 0;
		std::map<uint8_t, uint16_t> peerInputDelayFrames; // Per-sender delay by peerId; empty = every peer uses inputDelayFrames.
		std::map<uint8_t, uint32_t> peerIncarnations;
		std::map<uint8_t, uint64_t> initialPeerLeaves;
		std::map<uint8_t, std::map<uint64_t, uint16_t>> initialDelayChanges;
		std::map<uint8_t, NetGameSeatHold> initialSeatHolds;
		std::map<uint8_t, NetGameSeatReclaim> initialSeatReclaims;
		uint64_t seatStateThroughFrame = 0; // A joining round's holds, departures and returns already cover every frame up to this one.
		uint32_t timeoutMs = 500;
		uint8_t localPeerId = 0;
		uint8_t remotePeerId = 0; // 2-peer convenience; N-peer derives the remote set from peerCount.
		uint8_t peerCount = 2;
		NetPeerId remoteTransportPeerId = c_InvalidNetPeerId; // 2-peer convenience; see remoteTransportPeerIds.
		std::map<uint8_t, NetPeerId> remoteTransportPeerIds; // Lockstep peerId -> transport id for each remote; empty = derive the 2-peer pair.
		bool relayToOtherPeers = false; // Host-star: this (host) peer forwards each remote's frames/checksums to the other remotes.
		NetTransportLane frameLane = NetTransportLane::ControlReliable;
		std::string scenario = "lockstep";
		std::string ownershipPolicy = "unique-id-split";
		NetMatchConfig matchConfig;
		uint64_t roundId = 0; //!< Host: a fresh nonzero tag per round. Client: 0, adopted from the host's start.
		std::array<uint8_t, 16> seatPresenceEpoch{}; //!< The epoch already established by admission; zero disables roster packets.
		bool resumeFromSnapshot = false;
		uint8_t authorityPeerId = 0;
		uint64_t migrationGeneration = 0;
		std::vector<uint8_t> activePeerIds;
		std::array<uint8_t, 32> migrationKey{};
		std::function<std::unique_ptr<INetTransport>()> migrationTransportFactory;
		/// How many ticks a negotiated window packet repeats. 1 keeps the classic one-tick send; 0 uses 4.
		uint8_t frameRedundancyTicks = 1;
		// An active world's joiner owes every remote's input from startFrame without delay ramp-in.
		bool joinsRunningRound = false;
		std::optional<NetHash32> originalRoundConfigHash;
		bool adaptiveInputDelay = false;
		double simTickMs = 0;
		std::map<uint8_t, NetInputDelayEstimator> initialDelaySamples;
		std::function<void(const NetMatchConfig&)> publishLiveConfig;
		bool substituteSlowPeers = false;
		uint16_t slowPlayerBoundTicks = NetMatchConfigUtil::c_DefaultSlowPlayerBoundTicks;
		// Service matches wait for every peer's measured activity startup before the agreed first frame.
		bool requirePublishedStart = false;
	};

	enum class NetHostMigrationPhase : uint8_t {
		None,
		Contacting,
		Recovering,
		WaitingForReady,
		ResyncAdmission,
		Complete,
		Failed
	};
	enum class NetHostMigrationMessageType : uint16_t {
		Hello = 1,
		RollCall,
		Answer,
		Plan,
		RequestInput,
		Input,
		Ready,
		Commit,
		Rejoin,
		Abort
	};

	struct NetHostMigrationMessage {
		NetHostMigrationMessageType type = NetHostMigrationMessageType::Hello;
		uint64_t sessionId = 0;
		uint64_t roundId = 0;
		uint64_t generation = 0;
		uint8_t senderPeerId = 0;
		uint8_t successorPeerId = 0;
		NetHash32 configHash{};
		uint64_t appliedFrame = 0;
		uint64_t completeFrom = 0;
		uint64_t boundary = 0;
		uint64_t frame = 0;
		uint32_t totalBytes = 0;
		uint32_t offset = 0;
		std::vector<uint8_t> members;
		std::vector<uint8_t> bytes;
		std::vector<NetLockstepTiming> futureDelays;
	};

	class NetHostMigrationCodec {
	public:
		static constexpr uint32_t c_Magic = 0x314D4843;
		static constexpr uint16_t c_Version = 2;
		static constexpr size_t c_ChunkBytes = 48 * 1024;
		static constexpr size_t c_MaxFrameBytes = 4 * 512 * 1024 + 256;
		static constexpr size_t c_HistoryFrames = 2 * 240;
		static constexpr size_t c_MaxHistoryBytes = 16 * 1024 * 1024; // Older inputs yield to resync before recovery consumes match memory.
		static bool LooksLikePacket(const std::vector<uint8_t>& bytes);
		static bool Encode(const NetHostMigrationMessage& message, const NetHash32& key, std::vector<uint8_t>& bytes);
		static bool Decode(const std::vector<uint8_t>& bytes, const NetHash32& key, NetHostMigrationMessage& message);
	};

	struct NetHostMigrationResult {
		uint64_t generation = 0;
		uint64_t boundary = 0;
		uint8_t hostPeerId = 0;
		std::vector<uint8_t> members;
		std::vector<uint8_t> resyncPeers;
		std::map<uint8_t, NetPeerId> transports;
		uint8_t snapshotProviderPeerId = 0;
	};

	struct NetLockstepReadyFrame {
		uint64_t frame = 0;
		std::vector<uint8_t> departedPeerIds;
		std::vector<uint8_t> aiHeldPeerIds;
		std::vector<uint8_t> reclaimedPeerIds;
		std::map<uint8_t, uint64_t> committedPeerLeaves;
		std::map<uint8_t, uint64_t> committedFrameWaivers;
		bool hasLocalInput = false;
		std::map<uint8_t, size_t> remoteFrameCounts;
		std::vector<ControllerFrame> localFrames;
		std::vector<ControllerFrame> remoteFrames;
		std::vector<NetGameCommand> localCommands;
		std::vector<NetGameCommand> remoteCommands;
		std::vector<NetSoundObservation> localObservations;
		std::vector<NetSoundObservation> remoteObservations;
		std::vector<NetValueObservation> localValueObservations;
		std::vector<NetValueObservation> remoteValueObservations;
	};

	/// Applies the committed frame's departures before its game commands.
	void ApplyLockstepSeatReclaims(const NetLockstepReadyFrame& readyFrame, const std::deque<Actor*>& actors);
	void ApplyLockstepLeaveHandoffs(const NetLockstepReadyFrame& readyFrame, const std::deque<Actor*>& actors, bool paused);

	/// The applied frame with the frames a synced pause committed discounted: a pause commits frames the
	/// sim never advances on, and those must not spend a capped match's tick budget.
	uint64_t LockstepPlayedFrame();

	/// Clears the paused-frame discount a coordinator handoff or resync relaunch starts from zero.
	void ResetLockstepPausedFrames();
	uint64_t GetLockstepPausedFrames();
	void RestoreLockstepPausedFrames(uint64_t frames);

	struct NetLockstepPauseState {
		bool paused = false;
		int resumeCountdown = -1;
		uint64_t pausedFrames = 0;
		bool IsValid(uint64_t frame) const { return pausedFrames <= frame && (resumeCountdown == -1 || (paused && resumeCountdown > 0)); }
	};

	/// One remote's share of the round, enough to tell a peer that stopped SENDING from one the host
	/// stopped RELAYING to, and from one whose frames arrived and were refused.
	struct NetLockstepPeerStats {
		uint32_t framePacketsReceived = 0;
		uint64_t controllerFramesReceived = 0;
		uint32_t framesContributed = 0; //!< This peer's frames that reached a committed tick.
		uint32_t duplicateFrames = 0;
		uint32_t windowCopiesSkipped = 0; //!< Older window ticks already committed; not a loss.
		uint32_t windowCopiesApplied = 0; //!< Older window ticks that filled a hole.
		uint8_t lastFrameReserved = 0; //!< Reserved byte last encoded toward this peer.
		uint32_t outOfOrderFrames = 0;
		uint32_t futureFrameDrops = 0;
		uint32_t staleRoundPackets = 0;
		uint32_t preStartBuffered = 0;
		uint32_t relayPacketsSent = 0; //!< Host: packets forwarded TO this peer.
		uint32_t relaySendFailures = 0; //!< Host: forwards the transport refused for this peer.
		uint32_t relayResends = 0; //!< Host: refused forwards a later retry did deliver.
		uint32_t relayBacklogOverflows = 0; //!< Host: forwards this peer's full backlog could not hold.
		uint64_t longestCongestionHoldMs = 0; //!< Host: the longest this peer kept its seat behind our undrained queue.
		std::string lastRelayError; //!< Host: why a forward to THIS peer was last refused.
		uint64_t relayBytesSent = 0; //!< Host: encoded bytes forwarded to this peer, the send-buffer pressure it sees.
		uint32_t largestRelayPacketBytes = 0; //!< Host: the biggest single forward, so an oversized frame is visible.
		uint32_t relayBacklogPackets = 0; //!< Host: forwards still held for this peer.
		uint64_t highestTargetFrame = 0;
		uint64_t acceptedThroughFrame = 0; //!< The newest tick of this sender's the round could consume.
		uint64_t lastHeardMs = 0;
		uint64_t lastProgressMs = 0; //!< When this peer last raised the newest tick it has sent us.
		uint64_t reclaimAdmittedMs = 0; //!< When this seat's reclaim was admitted; its allowance runs from here.
		uint64_t returnerCaughtUpMs = 0; //!< When this returning seat's catch-up reached its reclaim frame; 0 while it has not.
		bool returnsInPlace = false; //!< This seat's return replays on its own state and connection: it starts its round before its reclaim frame.
		uint64_t startParkMs = 0; //!< The start work THIS peer's machine measured, as it published it.
		uint32_t pingMs = 0;
		uint32_t jitterMs = 0;
		uint16_t delayFrames = 0;
		uint64_t reportedNextFrame = 0;
		uint64_t longestWaitMs = 0;
		uint32_t waits = 0;
		uint32_t holds = 0;
		uint32_t substitutions = 0;
		uint32_t rejoins = 0;
		uint64_t longestWaitMsSinceReclaim = 0; //!< What this seat has waited since it was last reclaimed; the match record above keeps the round's totals.
		uint32_t waitsSinceReclaim = 0;
	};

	struct NetLockstepStats {
		uint64_t sessionId = 0;
		uint64_t configuredStartFrame = 0;
		uint64_t effectiveStartFrame = 0;
		uint16_t inputDelayFrames = 0;
		uint8_t localPeerId = 0;
		uint8_t remotePeerId = 0;
		uint32_t startPacketsSent = 0;
		uint32_t startPacketsReceived = 0;
		uint32_t framePacketsSent = 0;
		uint32_t framePacketsReceived = 0;
		uint32_t ignoredSessionPackets = 0;
		uint32_t ignoredAdmissionFaults = 0; //!< Unbound-transport faults/garbage dropped without touching the running match.
		uint32_t staleRoundPackets = 0; //!< Packets tagged with another lockstep round, ignored.
		uint32_t startRetransmits = 0; //!< Starts re-sent while waiting, or on a peer's repeated start.
		uint32_t startAnswers = 0; //!< The share of those that answered a peer's repeat rather than the ladder.
		uint32_t roundReadoptions = 0; //!< Rounds this peer followed the host onto after taking an older one.
		uint32_t startAnswersSuppressed = 0; //!< Repeated starts left unanswered: their sender had already played this round.
		uint32_t startsRelayedOnRepeat = 0; //!< Host: other remotes' starts re-sent to a peer that repeated its own.
		uint32_t preStartFramesBuffered = 0; //!< Frames/checksums held until their sender's start arrived.
		uint64_t checksumSubmissions = 0; //!< Local desync-check hashes the round took; a zero means the check never ran.
		uint64_t checksumSends = 0; //!< Those the transport carried to the peers.
		uint64_t checksumCompares = 0; //!< Local/remote hash pairs actually compared, per remote.
		uint64_t checksumMismatches = 0; //!< Compares that named a desync.
		uint64_t localControllerFramesSent = 0;
		uint64_t remoteControllerFramesReceived = 0;
		uint64_t remoteControllerFramesAccepted = 0;
		uint32_t framesAccepted = 0;
		uint32_t duplicateFrames = 0;
		uint32_t windowCopiesSkipped = 0;
		uint32_t windowCopiesApplied = 0;
		uint32_t outOfOrderFrames = 0;
		uint32_t frameBindingGapDrops = 0; //!< Unreliable frames whose bindings this peer missed past every window.
		uint32_t futureFrameDrops = 0; //!< Frames beyond the skew window, dropped so the maps stay bounded.
		uint32_t missingFrameStalls = 0;
		uint32_t blockingFrameWaits = 0;
		uint64_t holdNoticeBudgetMs = 0;
		bool holdDeadlineFeasible = true;
		uint64_t lastHoldDeclarationMs = 0;
		uint32_t ownParksExcluded = 0; //!< Gaps in our own ticks that were not charged to a peer.
		uint64_t longestOwnParkMs = 0; //!< The longest of them; the start work a peer's machine is also doing.
		uint64_t localTickOverruns = 0;
		uint64_t localLateInputs = 0;
		uint32_t consecutiveLateInputs = 0;
		double localComputeDebtMs = 0;
		double localProductionLateMs = 0;
		bool localMachineSlow = false;
		std::optional<uint32_t> measuredMissingFrameBase;
		std::optional<uint32_t> measuredBlockingWaitBase;
		uint32_t delayChangesProposed = 0;
		uint32_t delayChangesCommitted = 0;
		uint32_t delayPaddingFrames = 0;
		uint32_t delayDeferredSamples = 0;
		uint32_t relayPacketsSent = 0; //!< Host-star: forwards this peer made on behalf of another.
		uint32_t relaySendFailures = 0; //!< Forwards the transport refused; on a reliable lane the receiver never recovers them.
		uint32_t relayResends = 0; //!< Refused forwards a later retry did deliver.
		uint32_t relayCongestedRefusals = 0; //!< Forwards refused because OUR queue was full, not because the peer went.
		uint32_t relayCongestionHolds = 0; //!< Peers held through a congestion episode instead of being dropped.
		uint64_t longestCongestionHoldMs = 0; //!< The longest a peer kept its seat behind our undrained queue.
		uint32_t relayBacklogOverflows = 0; //!< Forwards a full backlog could not even hold.
		uint64_t relayBytesSent = 0; //!< Encoded bytes this host forwarded, across every peer.
		uint32_t largestRelayPacketBytes = 0;
		uint64_t relayBacklogBytes = 0; //!< Bytes still held for peers whose forwards were refused.
		uint64_t observationsCarried = 0; //!< Times a full frame left a reading for the next one to carry.
		uint64_t observationsDropped = 0; //!< Carried readings dropped because new sounds outran the wire for frames on end.
		uint64_t valueObservationsCarried = 0;
		uint64_t valueObservationsDropped = 0;
		uint32_t unresolvedObservationPackets = 0; //!< Frames dropped because an observation named a slot this peer never got.
		uint32_t relayObservationOverflows = 0; //!< Forwards that could not carry a frame's whole observation set; the tables would disagree.
		uint32_t peersDroppedSilent = 0; //!< Remotes the host adjudicated gone for going quiet, not for closing their socket.
		uint32_t stopsFromLeftPeers = 0; //!< Stops a peer sent after the round had already dropped its seat.
		uint32_t stopsAdjudicatedAsLeaves = 0; //!< Non-recovery client Stops the relay host treated as that client's leave.
		uint32_t peerFramesWaived = 0; //!< Fenced incarnations the round stopped requiring frames from; not seat drops.
		uint32_t connectionsClosedOnEviction = 0; //!< Connections the host closed because the round took the seat.
		uint32_t timeouts = 0;
		uint64_t parkFramesCommitted = 0; //!< Frames this round committed inside a capture park.
		uint32_t frameResendRequests = 0; //!< Resend requests this peer sent for ticks the unreliable lane lost.
		uint32_t framesResent = 0; //!< Own ticks this peer resent on the reliable lane on request.
		uint64_t parkFramesWithInput = 0; //!< Of those, the ones carrying every seat's input they required.
		uint64_t nextFrame = 0;
		uint64_t longestStallMs = 0;
		std::string lastMissingPeers; //!< Who the longest stall was waiting on.
		std::string lastRelayError;
		std::string timeoutReason;
		std::map<uint8_t, NetLockstepPeerStats> peers;
	};

	class NetLockstepCodec {
	public:
		static constexpr uint32_t c_Magic = 0x334C4343U;
		/// Version 39 carries each seat's device class in the start and the agreed-start record; admission refuses a peer below it.
		static constexpr uint16_t c_Version = 39;
		static constexpr uint16_t c_WorldVersion = 39;
		static constexpr uint16_t c_SeatDeviceVersion = 39;
		/// Version 37 carries input frames on the unreliable lane: a window reaches back a round trip, and a tick that
		/// arrives after this peer read past it is read past again rather than taken for a sender that started over.
		static constexpr uint16_t c_UnreliableFrameVersion = 37;
		/// Version 35 carries the host-authored agreed-start record after the ordinary Start fields.
		/// Older readers reject that packet as trailing bytes; they never interpret the record as a local start.
		static constexpr uint16_t c_AgreedStartVersion = 35;
		/// Version 36 names the proposal a re-stamped timing decision withdraws, so no peer keeps the old one.
		static constexpr uint16_t c_TimingWithdrawVersion = 36;
		static constexpr uint16_t c_InputAcceptanceVersion = 34;
		static constexpr uint16_t c_CheckpointVersion = 40; //!< The newest wire: frames that carry the checkpoint schedule.
		static constexpr uint16_t c_WorldAdmissionVersion = 28;
		static constexpr uint16_t c_TimingVersion = 24;
		static constexpr uint16_t c_HoldTransactionVersion = 26;
		/// Advertised in Ack.receivedMask; the older peer decodes the Ack and ignores receivedMask.
		static constexpr uint32_t c_FrameWindowCapabilityMask = 0x80000000U;
		static constexpr uint32_t c_InputAcceptedMask = 0x40000000U;
		/// Asks the named sender (the low byte) to resend its ticks from highestContiguousFrame on the reliable lane; an older peer ignores it.
		static constexpr uint32_t c_FrameResendRequestMask = 0x20000000U;
		static constexpr uint8_t c_MaxWindowTicks = 32;
		// Versions 8 and 9 have the same layout minus the AIEquip and AIOrder commands; recordings made under them still decode.
		// Version 11 adds the round tag to starts, frames and checksums, and sound observations to frames.
		// Version 12 adds the system-authored Reseat command.
		// Version 14 spells a sound observation's key once per sender and refers to it by slot after that.
		// Version 15 says how many keys the sender had spelled out before the packet, so a receiver that
		// missed one refuses instead of reading a reused slot as the key it held before.
		// Version 16 distinguishes a dropped peer from a clean leave; admission hashes this version.
		// Version 17 carries authenticated, complete seat-presence snapshots.
		// Version 18 carries each peer's local player bindings with its applied inputs.
		// Version 19 carries host hold resolutions (Reclaimed / Substituted / Expired) on the stop wire.
		// Version 20 carries local-AI number and string value observations next to the sound readings.
		// Version 21 appends writerUID on AIOrder; v<=20 still decodes with writerUID 0.
		// Version 22 carries the AI pass's script messages and gibs as commands, and the AIOrder op that
		// sets a move target; a peer below it never sent them, so it refuses them instead of guessing.
		// Recordings introduce world transitions at this version.
		static constexpr uint16_t c_WorldTransitionVersion = 23;
		static constexpr uint16_t c_HoldResolutionVersion = 19;
		static constexpr uint16_t c_PlayerBindingsVersion = 18;
		static constexpr uint16_t c_ValueObservationVersion = 20;
		static constexpr uint16_t c_AIOrderWriterVersion = 21;
		static constexpr uint16_t c_AIPassEventVersion = 22;
		static constexpr uint16_t c_PlaceBrainVersion = 22;
		static constexpr uint16_t c_SeatSnapshotVersion = 17;
		static constexpr uint16_t c_MinVersion = 8;
		static constexpr uint16_t c_RoundVersion = 11;
		static constexpr uint16_t c_StartParkVersion = 32;
		static constexpr uint16_t c_ObservationSlotVersion = 14;
		static constexpr uint16_t c_ObservationBindingSequenceVersion = 15;
		static constexpr size_t c_MaxObservationsPerPacket = NetSoundObservationDictionary::c_MaxSlots;
		// What one frame's observations may cost. The compact form makes 4096 of them about 21 KB, so a
		// frame that hits this is carrying keys nobody has seen before; the rest ride the next frame.
		static constexpr size_t c_MaxObservationBytesPerPacket = 24U * 1024U;
		// How much a sender may hold back for later. Reaching this needs thousands of sounds nobody has
		// heard before, every frame, for frames on end; past it the stalest readings go.
		static constexpr size_t c_MaxCarriedObservations = NetSoundObservationDictionary::c_MaxSlots;
		static constexpr size_t c_MaxValueKeyBytes = 256;
		static constexpr size_t c_MaxValueStringBytes = 4096;
		static constexpr uint16_t c_HeaderBytes = 16;
		static constexpr size_t c_MaxPayloadBytes = 64U * 1024U;
		static constexpr size_t c_MaxRecoveryInputBytes = 512U * 1024U;
		static constexpr size_t c_MaxRecoveryChunkBytes = c_MaxPayloadBytes - c_HeaderBytes - 36;
		static constexpr size_t c_MaxScenarioBytes = 128;
		static constexpr size_t c_MaxOwnershipPolicyBytes = 128;
		static constexpr size_t c_MaxDiagnosticBytes = 512;
		static constexpr size_t c_MaxFramesPerPacket = 512;
		static constexpr size_t c_MaxCommandsPerPacket = 256;
		static constexpr size_t c_MaxCargoPerDelivery = 64;
		// P26's per-seat ledger cap: the largest actor list a reseat can legitimately carry.
		static constexpr size_t c_MaxReseatActors = 512;
		static constexpr size_t c_MaxSoundSetPath = 32;
		static constexpr size_t c_MaxSoundStructureBytes = 65536;
		static constexpr uint16_t c_MaxInputDelayFrames = 60;
		static constexpr uint8_t c_MaxPeerCount = 16;
		// How far ahead of the committed frame a received frame/checksum may legitimately target
		// (input-delay lead plus jitter); anything beyond is dropped so one peer cannot grow the
		// per-frame maps without bound.
		static constexpr uint64_t c_MaxFutureFrameSkew = 4ULL * c_MaxInputDelayFrames;

		/// Whether a string can ride the wire as a field, asked with the encoder's own rule so a caller
		/// that must not queue an unsendable call never keeps a second copy of it.
		static bool IsWireString(const std::string& value, size_t maxBytes);

		static NetLockstepPacketType PacketTypeOf(const NetLockstepPayload& payload);
		static const char* PacketTypeName(NetLockstepPacketType type);
		static const char* StopReasonName(NetLockstepStopReason reason);
		static const char* ErrorCodeName(NetLockstepErrorCode code);

		/// Without a dictionary every observation spells out its key, so the packet stands alone; that is
		/// what a replay record and a one-shot round trip want. With one, only what fits the observation
		/// byte budget is encoded and outObservationsEncoded says how many, so the caller can carry the
		/// rest; the dictionary is touched only once the packet is certain to encode. With a block store the
		/// observations of a tick are encoded once and kept, and a window packet repeats those bytes rather
		/// than re-encoding them against a dictionary that has moved on.
		static bool Encode(const NetLockstepPacket& packet, std::vector<uint8_t>& outBytes, NetLockstepError* error = nullptr, NetSoundObservationDictionary* dictionary = nullptr, size_t* outObservationsEncoded = nullptr, size_t* outValueObservationsEncoded = nullptr, NetLockstepObservationBlocks* blocks = nullptr);
		static bool EncodeRecoveryInput(const NetLockstepFrame& frame, std::vector<uint8_t>& outBytes, NetLockstepError* error = nullptr);
		static bool DecodeRecoveryInput(const std::vector<uint8_t>& bytes, NetLockstepFrame& outFrame, NetLockstepError* error = nullptr);
		/// The frame version selects the ControllerFrame layout and semantics; a recording carries its own.
		static NetLockstepDecodeResult Decode(const uint8_t* data, size_t size, uint16_t controllerFrameVersion = ControllerFrame::c_Version, NetSoundObservationTables* tables = nullptr);
		static NetLockstepDecodeResult Decode(const std::vector<uint8_t>& bytes, uint16_t controllerFrameVersion = ControllerFrame::c_Version, NetSoundObservationTables* tables = nullptr);
		/// Whether these bytes are a lockstep packet at all, for the session and lobby wires that share
		/// the socket and need to tell another phase's traffic from garbage without decoding a payload
		/// they hold no observation tables for.
		static bool LooksLikePacket(const std::vector<uint8_t>& bytes);
	};

	/// What the H4 admission plane says about a seat mid-round. The round asks before it adjudicates a
	/// lost transport, so a superseded incarnation is not read as a leave and a dropped holder still
	/// inside its reclaim window does not end the match it left.
	struct NetLockstepSeatState {
		bool fencedTransport = false; //!< The transport is a superseded incarnation; the seat's holder is elsewhere.
		bool heldForReclaim = false;  //!< The seat is committed and may still come back.
	};

	class NetLockstepCoordinator {
	public:
		bool Start(INetTransport& transport, const NetLockstepConfig& config, std::string* error = nullptr);
		/// Starts in playback mode: no remotes, no handshake — every frame commits from the local
		/// queue, which the replay reader feeds through QueueReplayFrame.
		bool StartReplay(INetTransport& transport, const NetLockstepConfig& config, std::string* error = nullptr);
		/// Installs the recording's host-authored startup boundary before playback queues its first frame.
		bool ApplyReplayAgreedStart(const NetLockstepStart& start, std::string* error = nullptr);
		bool IsReplayPlayback() const { return m_Playback; }
		const std::optional<NetLockstepStart>& GetAgreedStartRecord() const { return m_AgreedStartRecord; }
		/// Feeds one recorded tick straight into the commit path: command senders preserved, no
		/// delay math, no wire — the replay's committed frame is exactly the recording's.
		bool QueueReplayFrame(uint64_t frame, std::vector<ControllerFrame> frames, std::vector<NetGameCommand> commands, std::string* error = nullptr, std::vector<NetSoundObservation> observations = {}, std::vector<NetValueObservation> valueObservations = {});
		/// Rewinds a playback coordinator to re-commit from an earlier frame (the rollback
		/// fidelity gate re-runs a window). Replay mode only — there is no wire to rewind.
		bool RewindReplay(uint64_t firstFrame, std::string* error = nullptr);
		bool QueueLocalInput(uint64_t producedFrame, const std::vector<ControllerFrame>& frames, const std::vector<NetGameCommand>& commands, std::string* error = nullptr, const std::vector<NetSoundObservation>& observations = {}, const std::vector<NetValueObservation>& valueObservations = {});
		bool PrimeResyncFrames(const std::vector<std::vector<NetGameCommand>>& batches, std::string* error = nullptr);
		bool PrimeResyncInputs(const std::vector<NetLockstepFrame>& batches, std::string* error = nullptr);
		bool InstallResyncInputs(const std::vector<NetLockstepFrame>& authoritativeInputs, std::string* error = nullptr);
		bool QueueRecoveredInput(const NetLockstepFrame& frame, std::string* error = nullptr);
		std::vector<NetLockstepFrame> CapturePendingInputs(uint64_t afterFrame) const;
		std::vector<NetLockstepFrame> CaptureLocalInputHistory() const;
		bool NeedsResyncPriming() const { return m_Config.resumeFromSnapshot && !m_ResyncPrimed; }
		bool SubmitLocalChecksum(uint64_t frame, const std::array<uint8_t, 32>& hash, std::string* error = nullptr, const std::map<uint8_t, uint64_t>& appliedCommands = {});
		const std::map<uint8_t, uint64_t>& GetAuthoritativeCommandAcks() const { return m_AuthoritativeCommandAcks; }
		std::vector<NetResyncPendingCommand> CapturePendingCommands(uint64_t afterFrame) const;
		std::vector<NetResyncPendingCommand> CapturePendingPlayerBindings(uint64_t afterFrame) const;
		void Tick(uint64_t nowMs);

		/// This machine's own measured start work, published so every peer judges us by it and not by theirs.
		void NoteLocalStartPark(uint32_t restartMs);
		/// This machine's seat device (Controller::WireDeviceClass), published with the start so the agreed record names every seat's.
		void NoteLocalDeviceClass(uint8_t deviceClass) { m_LocalDeviceClass = deviceClass; }
		/// The device class the agreed start names for a seat (a human slot in roster order); 0 before the record or for an unnamed seat.
		uint8_t AgreedSeatDeviceClass(int seat) const;
		/// Marks the agreed autosave tick as a local park while every peer captures the same state.
		void BeginSynchronizedCapture(uint64_t completedFrame);
		void CompleteSynchronizedCapture(uint64_t completedFrame, double captureMs);
		bool IsSynchronizedCapturePark(uint64_t frame) const;
		void Complete(const std::string& message = "complete");
		/// Announces a clean local leave: peers keep our frames through the last produced one, then
		/// advance without us. The relay host cannot leave a 3+ match alive (it is the star's hub),
		/// so a host leave completes the match for everyone instead.
		void Leave(const std::string& message = "player left");
		/// Ends the round on every peer so the match reconvenes and reloads the host's snapshot
		/// (a rejoin or an operator-forced heal). Host-initiated.
		void RequestResync(const std::string& message = "resync requested", bool immediate = false);
		/// Keeps recovery and completion aligned with applied simulation ticks, while input may be prefetched.
		void DeferStopsToTickBoundary() { m_DeferStops = true; }
		bool HasPendingRecoveryStop() const { return m_PendingRecoveryStop.has_value(); }
		/// The first frame the sim has not applied: a heal resumes the round here.
		uint64_t GetResumeFrame() const { return m_LastCompletedSimulationTick ? *m_LastCompletedSimulationTick + 1 : m_Config.startFrame; }
		/// Whether this peer has simulated any frame of the round.
		bool HasCompletedSimulationTick() const { return m_LastCompletedSimulationTick.has_value(); }
		bool FinishSimulationTick(uint64_t completedTick);
		/// Waives the parked tick's frames for every peer it still needs whose transport the admission
		/// plane has fenced or forgotten, so the tick commits and the pending stop fires at its boundary.
		/// The seat is untouched: the waived peer is a superseded incarnation, not a leaver.
		/// @return Whether a waiver was issued.
		bool WaivePendingPeersWhileWaiting(uint64_t waitingTick);
		/// Receives the session-protocol traffic (a reconnecting peer's handshake) the coordinator
		/// would otherwise discard while it owns the transport queue.
		void SetSessionEventSink(std::function<void(const NetTransportEvent&)> sink) { m_SessionEventSink = std::move(sink); }
		/// The H4 seat state, asked for by lockstep peer id and (on a disconnect) the transport that
		/// went away. Without one every seat reads as neither fenced nor held, which is the pre-H4 round.
		void SetSeatStateSource(NetLockstepSeatState (*source)(void*, uint8_t, NetPeerId), void* context);
		bool PopReadyFrame(NetLockstepReadyFrame& outFrame);
		//! The timing proposals this peer is holding, by revision.
		std::vector<uint64_t> PendingTimingRevisions() const {
			std::vector<uint64_t> revisions;
			for (const auto& [revision, decision]: m_TimingDecisions) revisions.push_back(revision);
			return revisions;
		}

		//! Frames committed and not yet consumed: the round's runway.
		size_t ReadyFrameCount() const { return m_ReadyFrames.size(); }
		bool HasReadyFrame(uint64_t frame) const { return !NeedsMigrationSnapshot() && !m_ReadyFrames.empty() && m_ReadyFrames.front().frame == frame; }
		/// The local frames already queued for a future frame; the local-actor preview runs them early.
		bool PeekLocalFrames(uint64_t frame, std::vector<ControllerFrame>& outFrames) const;
		bool PeekLocalInput(uint64_t frame, NetLockstepFrame& outFrame) const { return FindLocalInput(frame, outFrame); }
		/// The committed ready-frame for that tick, if it is still held or was just advanced.
		bool PeekReadyFrame(uint64_t frame, NetLockstepReadyFrame& outFrame) const;
		void RememberAppliedFrameInputs(const NetLockstepReadyFrame& ready);
		/// The in-flight commands this coordinator still holds for one seat at a frame.
		bool PeekQueuedCommands(uint64_t frame, uint8_t peerId, std::vector<NetGameCommand>& outCommands) const;
		uint16_t InputDelayAt(uint8_t peerId, uint64_t producedFrame) const;
		bool TimingDecisionPendingAt(uint64_t frame) const;
		/// Names every decision holding a frame's production, for a wait that has lasted long enough to be a defect.
		std::string DescribePendingTimingDecisions(uint64_t frame) const;
		bool DeferLocalInput(uint64_t producedFrame, const std::vector<ControllerFrame>& frames);
		bool ProposeInputDelay(uint8_t peerId, uint16_t delayFrames, uint64_t applyFrame, std::string* error = nullptr);
		bool ProposePeerHold(uint8_t peerId, uint64_t nowMs, std::string* error = nullptr);
		bool SchedulePeerReclaim(uint8_t peerId, NetPeerId transport, uint32_t incarnation, uint64_t frame, std::string* error = nullptr);
		bool ProposeWorldAdmission(NetPeerId transport, uint32_t incarnation, const NetGameWorldTransition& transition, std::string* error = nullptr);
		bool HasWorldAdmission(uint8_t peer, uint64_t frame) const { const auto it = m_ReclaimTransactions.find(peer); return it != m_ReclaimTransactions.end() && it->second.activationFrame == frame && it->second.worldTransition.has_value(); }
		void InjectEvent(const NetTransportEvent& event, uint64_t nowMs) { HandleEvent(event, nowMs); }
		/// Every peer's delay changes this round has applied, keyed by the frame each takes effect.
		const std::map<uint8_t, std::map<uint64_t, uint16_t>>& GetDelayChanges() const { return m_DelayChanges; }
		/// Marks the host's goodbye drain: the round has run its last tick and judges no seat from here.
		void SetGoodbyeDrain(bool draining) { m_GoodbyeDrain = draining; }
		/// The last frame this peer will simulate: every peer stops producing past it.
		void SetFinalFrame(uint64_t frame) { m_FinalFrame = frame; }
		bool NoteFrameWait(uint64_t frame, uint64_t nowMs, bool waitingForDecision = false);
		/// Moves every running deadline past a gap in our own ticks, so our park is not charged to a peer.
		void ShiftDeadlinesPastOurOwnPark(uint64_t nowMs);
		void FinishFrameWait(uint64_t nowMs);
		void NoteLocalTickCost(uint64_t producedFrame, double computeMs);
		void NoteLocalInputProduced(uint64_t producedFrame, uint64_t nowUs, uint64_t networkWaitUs);
		bool UsesBoundedWait() const { return m_Config.substituteSlowPeers; }
		const std::map<uint8_t, NetGameSeatHold>& HeldTransactions() const { return m_HoldTransactions; }
		/// Moves each seat the round took back before a joining seat's first frame out of the held state its replayed tail ended on.
		/// @param config The joining round's configuration; its holds, departures, incarnations and reclaims are updated.
		/// @param reclaims The host's ReclaimAtFrame decisions the joining seat has received.
		/// @param firstFrame The joining round's first frame.
		static void AdoptReturnsBefore(NetLockstepConfig& config, const std::vector<NetLockstepTiming>& reclaims, uint64_t firstFrame);
		bool HasAgreedSeatReclaim(uint8_t peer) const { return m_ReclaimTransactions.contains(peer); }
		/// What a seat has waited SINCE it was last reclaimed: what the player is shown, while the
		/// match record in GetStats()/BuildReportJson keeps the round's totals.
		uint32_t WaitsSinceReclaim(uint8_t peerId) const;
		uint64_t LongestWaitMsSinceReclaim(uint8_t peerId) const;
		/// Counts reclaims applied to the local seat; the surfaces restart their own clocks when it moves.
		uint32_t LocalSeatReclaims() const { return m_LocalSeatReclaims; }
		/// Restarts a returning seat's presentation readings at the frame its reclaim commits.
		void NoteSeatReclaimed(uint8_t peerId);
		/// Host: a returning seat whose catch-up is still replaying toward its reclaim frame; its first-input allowance starts at the catch-up's end.
		void NoteReturnerCatchingUp(uint8_t peerId, uint64_t nowMs);
		/// Host: a returning seat whose catch-up reached its reclaim frame; its first input is judged like any seat's from here.
		void NoteReturnerCaughtUp(uint8_t peerId, uint64_t nowMs);
		/// Host: a held seat that catches up in place on its own state and connection pays no restart, so none is owed to its return.
		void NoteInPlaceReturn(uint8_t peerId);
		/// A joining round whose seat state was read before its first frame takes every hold and return of another seat that the replay
		/// of its committed tail applied after that read, through the frame before its first; the host's own notice of them may never reach it.
		void AdoptReplayedSeatTransitions(const NetLockstepCoordinator& replay, uint64_t throughFrame);
		/// Host: a returning seat bound to the round again is sent every frame from its reclaim frame the round sent while it was away,
		/// so its live round starts with the inputs every other peer already holds. Returns how many went.
		size_t SendReturnerTheRoundFrom(uint8_t peerId, uint64_t fromFrame);
		/// Every peer captures at the end of each tick that is a multiple of this (0 = none): the host is busy there, not gone.
		void SetAnnouncedCaptureEvery(uint32_t every) { m_AnnouncedCaptureEvery = every; }
		/// A capture every peer takes at the end of the tick: the host's silence behind it is its capture, not its death.
		void NoteAnnouncedCapture(uint64_t tick);
		/// Client: the host was heard now; the gap since it was last heard is its talk jitter.
		void NoteAuthorityHeard(uint64_t nowMs);
		/// Whether the frame waited on is one the host produces only after a capture every peer announced.
		bool HostBusyWithAnnouncedCapture(uint64_t frame) const;
		const std::map<uint8_t, NetPeerId>& RemoteTransports() const { return m_RemoteTransports; }
		bool IsSeatUnderAI(uint8_t peerId, uint64_t frame) const;
		bool IsSeatHoldGap(uint8_t peerId, uint64_t frame) const;
		bool HasSeatHoldGap(uint64_t frame) const { for (const auto& [peer, hold]: m_AiHeldSeats) if (IsSeatHoldGap(peer, frame)) return true; return false; }
		bool IsSeatReclaimGap(uint8_t peerId, uint64_t frame) const;
		bool HasSeatReclaimGap(uint64_t frame) const {
			for (const auto& [peer, reclaim]: m_ReclaimTransactions) if (IsSeatReclaimGap(peer, frame)) return true;
			for (const auto& [peer, gap]: m_RetiredReclaimGaps) if (frame >= gap.first && frame <= gap.second) return true;
			return false;
		}
		/// A seat the AI holds for its returner. A released seat stays under the AI but no longer waits for anyone.
		bool HasHeldAISeat(uint8_t peerId) const { return m_AiHeldSeats.contains(peerId) && !m_ReleasedAiSeats.contains(peerId); }
		bool AnyHeldAISeat() const { return std::any_of(m_AiHeldSeats.begin(), m_AiHeldSeats.end(), [&](const auto& seat) { return !m_ReleasedAiSeats.contains(seat.first); }); }
		/// Whether the seat's hold was ended by a kick, a ban, a release or a clean leave: its units stay with the AI and a return is a new join.
		bool IsSeatReleased(uint8_t peerId) const { return m_ReleasedAiSeats.contains(peerId); }
		/// A seat's reclaim or admission is agreed and its activation frame is still ahead.
		bool HasPendingSeatActivation() const { for (const auto& [peer, reclaim]: m_ReclaimTransactions) if (reclaim.activationFrame >= m_Stats.nextFrame) return true; return false; }
		/// Host: the current capture park covers the frame or may still grow to cover it.
		bool CaptureParkMayReach(uint64_t frame) const;
		bool IsLocalSeatHeld() const { return m_LocalSeatHeld; }
		/// The frame the host held this peer's seat from; 0 when the hold was not taken on the wire (a closed link).
		uint64_t GetLocalHoldFrame() const { return m_LocalHoldFrame; }
		bool PreparePeerRejoin(uint8_t peerId, uint32_t rttMs, uint64_t nowMs, std::string* error = nullptr);
		/// Delay window a returning seat needs: the measured round trip plus the restart its first tick pays.
		uint32_t RejoinDelayFrames(uint8_t peerId, const NetInputDelayEstimator& estimate) const;
		std::vector<uint8_t> ResumePeerIds() const;

		NetLockstepState GetState() const { return m_State; }
		bool IsRunning() const { return m_State == NetLockstepState::Running; }
		bool HasReceivedAllRemoteStarts() const { return AllRemoteStartsReceived(); }
		bool IsFailed() const { return m_State == NetLockstepState::Failed; }
		bool IsStopped() const { return m_State == NetLockstepState::Stopped; }
		const NetLockstepStats& GetStats() const { return m_Stats; }
		/// True only when every remote advertised the frame-window bit and this peer repeats ticks.
		bool FrameWindowAgreed() const;
		/// How many observation keys this peer has spelled out for a sender's stream this round.
		uint64_t ObservationBindingsSpelled(uint8_t senderPeerId) const;
		/// The readings this peer held and then had to drop. Their sampler must forget it ever sent them,
		/// or it will not offer them again until the sound's audibility moves.
		std::vector<NetSoundObservation> TakeDroppedObservations();
		std::vector<NetValueObservation> TakeDroppedValueObservations();
		const NetLockstepConfig& GetConfig() const { return m_Config; }
		/// The round every accepted packet carries; 0 on a client until the host's start arrives.
		uint64_t GetRoundId() const { return m_RoundId; }
		const NetHash32& GetRoundConfigHash() const { return m_RoundConfigHash; }
		uint8_t GetHostPeerId() const { return m_Config.authorityPeerId != 0 ? m_Config.authorityPeerId : m_Config.matchConfig.hostPeerId; }
		bool IsMigrating() const { return m_MigrationPhase == NetHostMigrationPhase::Contacting || m_MigrationPhase == NetHostMigrationPhase::Recovering || m_MigrationPhase == NetHostMigrationPhase::WaitingForReady || m_MigrationPhase == NetHostMigrationPhase::ResyncAdmission; }
		bool IsMigrationCatchUp() const { return IsMigrating() && GetResumeFrame() <= m_MigrationBoundary; }
		NetHostMigrationPhase GetMigrationPhase() const { return m_MigrationPhase; }
		const NetHostMigrationResult& GetMigrationResult() const { return m_MigrationResult; }
		const std::string& GetMigrationAddress() const { return m_MigrationAddress; }
		static bool ConnectMigrationEndpoint(INetTransport& transport, const NetMatchMigrationPeer& peer, size_t& nextAddress, std::string& connectedAddress, std::string* error = nullptr);
		bool NeedsMigrationSnapshot() const { return m_MigrationResult.snapshotProviderPeerId != 0 && m_Config.localPeerId == GetHostPeerId(); }
		std::unique_ptr<INetTransport> TakeMigrationTransport() { return std::move(m_MigrationTransport); }
		bool TakeMigrationNotice() { return std::exchange(m_MigrationNotice, false); }
		std::vector<NetTransportEvent> TakeMigrationAdmissionEvents() { return std::exchange(m_MigrationAdmissionEvents, {}); }
		void FinishMigrationAdmission() {
			m_MigrationPhase = NetHostMigrationPhase::Complete;
			RequestResync("handover survivor admitted for snapshot", true);
		}
		/// A transport fault starts agreement without choosing a simulation departure.
		bool BeginHostMigration(uint64_t nowMs);
		bool BeginHostMigrationAfterHeal(uint64_t nowMs);
		bool IsLocalActor(int64_t actorUniqueID, int actorTeam, bool cpuControlled) const;
		/// The peer that produces the actor's frames under the match's ownership policy, leaves applied; every peer resolves it identically.
		uint8_t ResolveActorOwner(int64_t actorUniqueID, int actorTeam, bool cpuControlled) const;
		/// The same, with leaves NOT applied: who HELD the actor, which is what the drop ledger records.
		uint8_t ResolveActorOwnerBeforeLeaves(int64_t actorUniqueID, int actorTeam, bool cpuControlled) const;
		uint8_t ResolveTeamCommandAuthority(int team) const;
		/// Whether a transport peer carries one of this round's lockstep remotes (a NEW transport
		/// peer reaching session-Ready mid-match is a reconnector).
		bool UsesTransportPeer(NetPeerId transportPeerId) const;
		/// Whether the actor's owner peer has left as of the given frame; the lockstep gate means every
		/// survivor answers this identically when consuming that frame, so the stand-down is synced.
		bool IsActorOwnerGone(int64_t actorUniqueID, int actorTeam, bool cpuControlled, uint64_t frame) const;
		/// Whether the peer has cleanly left as of the given frame (never true for the local peer).
		bool IsPeerGoneAtFrame(uint8_t peerId, uint64_t frame) const;
		/// Peers that announced a clean leave, each with the first frame that lacks their data.
		const std::map<uint8_t, uint64_t>& GetPeerLeaveFrames() const { return m_PeerLeaveFrames; }
		/// Fenced incarnations the round no longer waits on, each with the first frame it stopped needing.
		const std::map<uint8_t, uint64_t>& GetPeerFrameWaivers() const { return m_PeerFrameWaivers; }
		/// Whether the round is only still alive because a dropped seat may still be reclaimed: every
		/// remote has left and at least one of their seats is inside its window. Nobody can disagree
		/// with this peer about it, because while it holds there is no other peer in the round.
		bool IsHoldingSeatForReclaim() const;
		/// Whether this peer's left seat is still held, from the same set AnyLeftSeatHeld reads.
		bool IsSeatHeldForReclaim(uint8_t peerId) const;
		// Kept for UI estimates that still speak in frames (HoldSeconds(1200) == 20). The hold itself
		// is the admission wall-clock; commits do not advance while a dropped seat is unresolved.
		static constexpr uint64_t c_ReclaimHoldFrames = 1200;
		static constexpr uint64_t c_HoldPauseMs = 20000;
		/// How long past the host's own startup a bounded-wait round waits for a slow loader before the AI takes its seat.
		static constexpr uint64_t c_StartupAnswerBudgetMs = 5000;
		/// A sender's first second of play is judged by its startup ramp, not the bare slow-player bound.
		static constexpr uint64_t c_StartupSettleTicks = 60;
		static constexpr uint64_t c_HoldHeartbeatMs = 50;
		/// Marks a PeerDropped notice that waives a fenced incarnation's frames instead of dropping its seat.
		static constexpr std::string_view c_FrameWaiverPrefix = "fenced:";
		/// Whether any dropped seat is still waiting on a host resolution. The frame argument is the
		/// applied tick the activity gate names; the answer no longer moves with a frame deadline.
		bool IsSeatHeldForReclaimAtFrame(uint64_t frame) const;
		bool AnyDroppedSeatHeld() const { return !m_DroppedSeats.empty(); }
		NetLockstepHoldResolution HeldSeatResolution(uint8_t peerId) const;
		/// Host: end one held seat and tell every peer at the held frame.
		void ResolveHeldSeat(uint8_t peerId, NetLockstepHoldResolution resolution, uint64_t nowMs);

		/// Whether this round is a persistent world: nobody's departure ends it, and its membership is
		/// admitted one member at a time instead of being derived from the configured peer count.
		bool IsPersistentWorldRound() const { return m_Config.matchConfig.persistentWorld; }
		/// Adds an activated world member to the round. Its frames become required at firstRequiredFrame
		/// and not before, so the announced activation tick is exactly when the world starts waiting on it.
		/// @param peerId The lockstep id the world's slot table gave the member.
		/// @param transportPeerId The live connection the member's frames arrive on.
		/// @param firstRequiredFrame E: the first frame this member must produce.
		/// @return Whether the member was added.
		bool AdmitWorldMember(uint8_t peerId, NetPeerId transportPeerId, uint64_t firstRequiredFrame, std::string* error = nullptr);
		/// The highest target this peer has already put on the wire; 0 before its first input. An
		/// activation is announced ahead of it so the member is a peer before those frames go out.
		uint64_t SentInputThrough() const { return m_LastQueuedTargetFrame == UINT64_MAX ? 0 : m_LastQueuedTargetFrame; }
		/// The frame every sender spells its observation keys out from again, so a member admitted
		/// there decodes them with the empty table it starts with. 0 when no activation is pending.
		uint64_t ObservationEpoch() const { return m_ObservationEpochs.empty() ? 0 : *m_ObservationEpochs.rbegin(); }
		/// Every restart still announced, oldest first. Two joiners in flight announce two.
		const std::set<uint64_t>& ObservationEpochs() const { return m_ObservationEpochs; }
		/// Announces one restart: from this frame every sender spells its observation keys out again,
		/// so a member admitted there reads them with the empty table it starts with.
		void SetObservationEpoch(uint64_t frame);
		/// Moves one announced restart to a new frame, for a re-announce of the same activation.
		void MoveObservationEpoch(uint64_t from, uint64_t to);
		/// How many frames the last admission replayed to the member it admitted.
		size_t LastAdmissionReplayFrames() const { return m_LastAdmissionReplayFrames; }
		/// Whether the peer is a member the round waits on right now.
		bool IsWorldMember(uint8_t peerId) const { return IsKnownRemotePeer(peerId); }
		/// Host: remove one remote as a clean leave. A held seat expires; a live seat never opens a hold.
		void EvictRemovedPeer(uint8_t peerId, const std::string& message, uint64_t nowMs);
		uint64_t HoldPauseRemainingMs(uint64_t nowMs) const;
		std::string DescribeHeldPause(uint32_t& secondsLeft, uint64_t nowMs) const;
		/// Publishes one complete current view. Refused sends retry the latest view without growing a queue.
		bool PublishSeatSnapshot(std::vector<NetSeatPresenceEntry> seats, uint64_t observedAtMs);
		std::optional<NetLockstepSeatSnapshot> TakeSeatSnapshot();
		/// Whether the round has yet to commit a frame. A resync relaunch lands here: the ledgered
		/// reseat rides the first committed frame, so nothing the round produced can be judged before it.
		bool HasCommittedAFrame() const { return m_Stats.framesAccepted > 0; }
		/// Whether this relay host still owes a peer a forward it has not managed to send. The star's
		/// hub cannot leave while this is true: a client waiting on that frame loses the round.
		bool HasPendingRelayWork() const { return m_RelayHost && (!m_RelayBacklog.empty() || !m_RecoveryOutgoing.empty()); }
		/// Whether a live remote has not reported reaching the frame this peer has committed to. A peer merely
		/// behind owes nothing to the relay queues, so the goodbye drain would leave while it still needs us.
		/// How far every live remote has told us it has come.  The goodbye drain watches this for progress
		/// instead of spending a fixed budget on a peer that is never going to answer.
		uint64_t RemoteProgressSum() const {
			uint64_t sum = 0;
			for (const auto& [peer, stats]: m_Stats.peers) {
				if (peer == m_Config.localPeerId) continue;
				sum += stats.reportedNextFrame + stats.acceptedThroughFrame + stats.highestTargetFrame;
			}
			return sum;
		}
		bool HasPeerBehindOurHorizon() const {
			if (!IsRunning()) return false;
			for (uint8_t peer: m_RemotePeerIds) {
				if (IsPeerGoneAtFrame(peer, m_Stats.nextFrame)) continue;
				const auto stats = m_Stats.peers.find(peer);
				if (stats == m_Stats.peers.end() || stats->second.reportedNextFrame < m_Stats.nextFrame) return true;
			}
			return false;
		}
		/// Names the required peers the next frame still waits on; empty when none are missing.
		std::string DescribeMissingPeers() const;
		/// The peer's roster display name, or "peer N" when the roster has none.
		std::string DescribePeer(uint8_t peerId) const;
		std::string BuildReportJson() const;

		static const char* StateName(NetLockstepState state);

		friend bool TestDelayPaddingPassesAParkedFrame(std::string* error);
		friend bool TestACaptureReportsToItsOwnPark(std::string* error);
		friend bool TestALateStartsReclaimIsRetriedUntilAdmitted(std::string* error);
		friend bool TestHoldResolutionPumpDoesNotRelock(std::string* error);
		friend bool TestALongLinkedSurvivorDoesNotCollapseTheBound(std::string* error);
		friend bool TestAStarvedSeatIsNotLate(std::string* error);
		friend bool TestASurvivorsRunwayIsTheRounds(std::string* error);
		friend bool TestTheGoodbyeDrainJudgesNoSeat(std::string* error);
		friend bool TestNoSeatIsJudgedPastTheLastTick(std::string* error);
		friend bool TestAReturningSeatsRampIsTheBound(std::string* error);
		friend bool TestASeatIsNotLateForOurOwnDecision(std::string* error);
		friend bool TestAFirstDelayChangeIsNotAMutualWait(std::string* error);
		friend bool TestPendingSessionEventSurvivesTeardown(std::string* error);
		friend bool TestFinishMatchDrainsFencedDisconnect(std::string* error);
		friend bool TestServiceKick(std::string* error);
		friend bool TestAWorldAdmissionClearsAReleasedSeat(std::string* error);

	private:
		void TickHostMigration(uint64_t nowMs);
		void TickMigrationRollCallLinks(uint64_t nowMs);
		void HandleMigrationEvent(const NetTransportEvent& event, uint64_t nowMs);
		bool SendMigration(NetPeerId peer, NetHostMigrationMessage message);
		NetHostMigrationMessage MigrationMessage(NetHostMigrationMessageType type) const;
		bool ContactMigrationSuccessor(uint64_t nowMs);
		bool RestartHostMigrationAfterSuccessorLoss(uint64_t nowMs);
		bool IsLostMigrationSuccessor(uint8_t peerId) const;
		uint64_t MigrationStepBudgetMs() const { return std::clamp<uint32_t>(m_Config.timeoutMs, 1, 1000); }
		bool HoldsLiveMigrationCandidate(uint64_t nowMs, uint64_t budget) const;
		void PublishMigrationPlan(uint64_t nowMs);
		void CompleteHostMigration(uint64_t nowMs);
		void ApplyMigrationMembership(uint64_t nowMs);
		void FailHostMigration(const std::string& reason);
		bool EncodeMigrationFrame(const NetLockstepReadyFrame& frame, std::vector<uint8_t>& bytes) const;
		bool DecodeMigrationFrame(const std::vector<uint8_t>& bytes, uint64_t frame, NetLockstepReadyFrame& ready) const;
		void SendMigrationFrame(NetPeerId peer, uint64_t frame);
		void RetainMigrationFrame(const NetLockstepReadyFrame& ready);
		void StoreMigrationFrame(uint64_t frame, std::vector<uint8_t> bytes);
		NetHostMigrationPhase m_MigrationPhase = NetHostMigrationPhase::None;
		std::unique_ptr<INetTransport> m_MigrationTransport;
		std::unique_ptr<INetTransport> m_MigrationListener;
		struct MigrationProbe {
			std::unique_ptr<INetTransport> transport;
			size_t nextAddress = 0;
			uint64_t lastDialMs = 0;
			std::string address;
			bool answered = false;
		};
		std::map<uint8_t, MigrationProbe> m_MigrationProbes;
		size_t m_MigrationNextAddress = 0;
		std::string m_MigrationAddress;
		uint64_t m_MigrationGeneration = 0;
		uint64_t m_MigrationWireRound = 0;
		uint64_t m_MigrationSinceMs = 0;
		uint64_t m_MigrationStartedMs = 0;
		uint64_t m_MigrationLastSendMs = 0;
		uint64_t m_MigrationBoundary = 0;
		uint64_t m_MigrationFirstNeeded = 0;
		size_t m_MigrationCandidateIndex = 0;
		uint8_t m_MigrationSuccessor = 0;
		uint8_t m_MigrationDonor = 0;
		NetPeerId m_MigrationHostTransport = c_InvalidNetPeerId;
		bool m_MigrationNotice = false;
		bool m_MigrationNeedsResync = false;
		bool m_MigrationCommitQueued = false;
		bool m_MigrationAuthoritySeen = false;
		//!< Successors this peer gave up on; a later election in this handover chain must not dial them again.
		std::vector<uint8_t> m_MigrationLostSuccessors;
		NetHostMigrationResult m_MigrationResult;
		std::set<uint8_t> m_MigrationExpected;
		std::map<uint8_t, NetHostMigrationMessage> m_MigrationAnswers;
		std::vector<NetLockstepTiming> m_MigrationFutureDelays;
		std::map<uint8_t, NetPeerId> m_MigrationPeers;
		std::set<uint8_t> m_MigrationReady;
		std::map<uint64_t, std::vector<uint8_t>> m_MigrationHistory;
		size_t m_MigrationHistoryBytes = 0;
		std::map<uint64_t, std::vector<uint8_t>> m_MigrationIncoming;
		std::map<NetPeerId, std::deque<std::vector<uint8_t>>> m_MigrationOutbox;
		std::deque<std::tuple<NetPeerId, uint64_t, size_t>> m_MigrationFrameQueue;
		std::vector<NetTransportEvent> m_MigrationAdmissionEvents;
		std::vector<NetTransportEvent> m_MigrationEarlyInputs;
		bool QueueInputAtTarget(uint64_t targetFrame, const std::vector<ControllerFrame>& frames, const std::vector<NetGameCommand>& commands, std::string* error, const std::vector<NetSoundObservation>& observations, const std::vector<NetValueObservation>& valueObservations = {});
		/// Sends to every remote, or to one when onlyPeerId names it.
		bool SendPacket(const NetLockstepPacket& packet, NetTransportLane lane, std::string* error = nullptr, NetSoundObservationDictionary* dictionary = nullptr, size_t* outObservationsEncoded = nullptr, uint8_t onlyPeerId = 0, size_t* outValueObservationsEncoded = nullptr, NetLockstepObservationBlocks* blocks = nullptr);
		void HandleEvent(const NetTransportEvent& event, uint64_t nowMs);
		void HandlePacket(const NetLockstepPacket& packet, uint64_t nowMs, NetPeerId fromTransport);
		void HandleStart(const NetLockstepStart& start, uint64_t nowMs, NetPeerId fromTransport);
		void HandleFrame(const NetLockstepFrame& frame, uint64_t nowMs, NetPeerId fromTransport, bool relay = true, bool recovered = false);
		void HandleRecoveryChunk(const NetLockstepRecoveryChunk& chunk, uint64_t nowMs, NetPeerId fromTransport);
		bool RetainRecoveryInput(const NetLockstepFrame& frame, std::vector<uint8_t> bytes, bool commitLocal, std::string* error);
		void FlushRecoveryInputs(uint64_t nowMs = UINT64_MAX);
		void RememberLocalInput(const NetLockstepFrame& frame);
		bool FindLocalInput(uint64_t targetFrame, NetLockstepFrame& out) const;
		void AdvertiseFrameWindow();
		void HandleAck(const NetLockstepAck& ack, NetPeerId fromTransport);
		void AcknowledgeAcceptedInput(uint8_t peerId, uint64_t frame);
		bool FrameWindowAllRemotesAdvertised() const;
		bool FrameWindowAgreedFor(uint8_t peerId) const;
		uint8_t ConfiguredWindowTicks() const;
		void AttachFrameWindow(NetLockstepFrame& packet) const;
		/// Sends the padding a local delay rise owes before the tick that would wait for it.
		void PadAheadOfDelayRise();
		/// Asks for a sender's missing tick on the reliable lane, at most once a tick: a blip the window cannot bridge is not a hold.
		void RequestMissingFrames(uint8_t senderPeerId, uint64_t frame, uint64_t nowMs, uint64_t waitedMs);
		/// Resends this peer's own ticks from a frame to one peer on the reliable lane, repeating each tick's first bytes.
		size_t ResendOwnFramesFrom(uint8_t requesterPeerId, uint64_t fromFrame);
		/// Relay host: resends the ticks it forwarded for another sender, from a frame, to the peer that asked.
		size_t ResendRelayedFramesFrom(uint8_t requesterPeerId, uint8_t senderPeerId, uint64_t fromFrame);
		/// Sends one admitted member everything this peer still holds for targets from its first
		/// required frame to the highest already sent, in target order, own frame before the members'.
		size_t ReplaySentFramesTo(uint8_t peerId, uint64_t fromFrame);
		/// Rebuilds one sender's pending frame for a target from the stores the commit drains.
		bool BuildPendingRemoteFrame(uint64_t targetFrame, uint8_t senderPeerId, NetLockstepFrame& out) const;
		/// Resets one sender's encode table at the epoch, once, before its first frame at or past it.
		void ApplyObservationEpoch(uint8_t senderPeerId, uint64_t targetFrame);
		/// Forgets a restart every sender has already reset at, so the set stays the live ones.
		void PruneObservationEpochs();
		/// The block store of one sender, trimmed to the ticks a window can still repeat.
		NetLockstepObservationBlocks& ObservationBlocksOf(uint8_t senderPeerId, uint64_t newestTargetFrame);
		void AcceptRemoteTick(const NetLockstepFrame& frame, uint64_t nowMs, bool windowCopy);
		uint8_t LockstepPeerOfTransport(NetPeerId transportPeerId) const;
		bool SendStart(std::string* error, uint8_t onlyPeerId = 0);
		/// Sends a peer that repeated its start what it needs to form the round.
		void AnswerRepeatedStart(uint8_t peerId, uint64_t nowMs);
		/// Whether we take our round from that sender: the peer we are connected to. Everyone else's
		/// start reaches us relayed, carrying the round its sender adopted rather than the host's.
		bool IsRoundAuthority(uint8_t peerId, NetPeerId fromTransport) const;
		/// Whether a start describes the round this peer is configured for, field by field.
		bool StartMatchesConfig(const NetLockstepStart& start) const;
		/// Names the fields a refused start disagreed on, so a protocol error says what it saw.
		std::string DescribeStartMismatch(const NetLockstepStart& start) const;
		/// Leaves the round we formed for the one the host is in, keeping our own production.
		void ReadoptRound(uint64_t roundId, uint64_t nowMs);
		/// Delivers the frames and checksums a peer sent before its start reached us.
		void FlushPreStart(uint8_t peerId, uint64_t nowMs);
		/// Clears everything one round owns, so leaving a round cannot carry a fact from it.
		void ResetRoundState();
		/// Sends the production a followed round owes the host, in order, retrying a refused send.
		void FlushResendFrames();
		void HandleStop(const NetLockstepStop& stop, uint64_t nowMs, NetPeerId fromTransport);
		void HandleSeatSnapshot(const NetLockstepSeatSnapshot& snapshot, NetPeerId fromTransport);
		void FlushSeatSnapshot();
		void HandleChecksum(const NetLockstepChecksum& checksum, NetPeerId fromTransport);
		/// Whether a packet's claimed sender owns the transport it arrived on. Only the relay host
		/// receives each remote directly; clients get everything via the relay and trust the host.
		bool SenderOwnsTransport(uint8_t claimedPeerId, NetPeerId fromTransport) const;
		void CompareChecksums(uint64_t frame);
		void AdvanceReadyFrames(uint64_t nowMs);
		void ApplyPeerLeave(uint8_t peerId, uint64_t firstFrameWithout, const std::string& message, uint64_t nowMs, bool announced, bool closeTransport = false, bool agreedBoundary = false, bool removed = false);
		void ApplyHoldResolution(uint8_t peerId, NetLockstepHoldResolution resolution, uint64_t nowMs, bool relay);
		/// Ends an AI-held seat's wait for its returner: an agreed reclaim still ahead of every peer is withdrawn, the AI keeps the units.
		void ReleaseHeldSeat(uint8_t peerId, uint64_t nowMs, bool relay);
		void MaybeSendHoldHeartbeats(uint64_t nowMs);
		static bool IsHoldResolutionReason(NetLockstepStopReason reason);
		static NetLockstepStopReason StopReasonOf(NetLockstepHoldResolution resolution);
		static NetLockstepHoldResolution HoldResolutionOf(NetLockstepStopReason reason);
		/// The first frame this peer has no data for, walking up from the committed one.
		uint64_t FirstFrameWithout(uint8_t peerId) const;
		/// How long the host lets a required remote go quiet before calling it gone. Half the
		/// missing-frame grace, so the relayed notice still has the other half to reach the survivors.
		uint64_t PeerSilenceLeaveMs() const { return m_Config.timeoutMs / 2; }
		/// How long a peer keeps its seat while OUR queue to it will not drain. Three quarters of the
		/// grace: past the silence bound, so a transient episode still costs nobody a seat, and a clear
		/// quarter short of the missing-frame timeout, so a hold can never be what ends the round.
		uint64_t CongestionHoldLeaveMs() const { return m_Config.timeoutMs * 3 / 4; }
		/// Relay host: a required remote that has blocked the round this long has left, whatever its
		/// socket still says. Waiting for the transport means waiting on the dead peer's own process.
		void AdjudicateSilentPeers(uint64_t nowMs);
		void CountRelaySent(NetLockstepPeerStats& peerStats, size_t bytes);
		uint64_t RelayBacklogBytes() const;
		void UpdateRelayBacklogBytes() { m_Stats.relayBacklogBytes = RelayBacklogBytes(); }
		uint32_t RelayBacklogPackets(uint8_t peerId) const;
		/// Records whether a refusal was our own full queue or a real fault.
		void NoteRelayRefusal(uint8_t peerId, bool congested);
		/// Records a refusal against the peer it was for as well as the round.
		void NoteRelayError(uint8_t peerId, const std::string& error);
		/// Drops every trace of a peer's congestion, so a reused id starts clean.
		void ForgetCongestion(uint8_t peerId);
		/// Holds a refused forward for retry, keeping this peer's stream in order behind it.
		void QueueRelayBacklog(uint8_t peerId, const std::vector<uint8_t>& bytes);
		/// Retries refused forwards. A momentarily full send buffer heals; one that stays refused past
		/// the silence budget is a gap the receiver can never fill, so that peer leaves the round.
		void FlushRelayBacklog(uint64_t nowMs);
		/// Drops peers whose forwards could not be delivered at all.
		void DropUnreachablePeers(uint64_t nowMs);
		/// Asks the match service for a seat. Only ever from inside Tick: the service pumps us with its
		/// own lock held, so asking it back from an ownership query re-locks that lock on its own thread.
		NetLockstepSeatState SeatStateOf(uint8_t peerId, NetPeerId transportPeerId) const;
		/// Re-resolves which left seats are still held. Runs from the tick, never from a query.
		void RefreshLeftSeatHolds();
		/// Whether any peer that has left still holds a seat a returning player can reclaim.
		bool AnyLeftSeatHeld() const;
		/// A resync round already named this peer; a leftover drop or the old socket's close is not a new hold.
		bool IgnoreStaleRefillLeave(uint8_t peerId, uint64_t nowMs) const;
		/// A Reclaimed or Substituted seat is being refilled; it is not a last-player close.
		bool SeatIsRefilling(uint8_t peerId) const;
		bool AnySeatRefilling() const;
		size_t LeftPeersNotRefilling() const;
		/// Ends a round every remote has left once the last held seat's reclaim window has closed.
		void EndRoundIfNobodyIsComingBack();
		/// Whether a scheduled resync owns the end of this round; a last-player leave must not take it.
		bool ReclaimResyncPending() const;
		bool IsRemoteRequiredForFrame(uint8_t peerId, uint64_t frame) const;
		/// Whether a committed frame carries this remote's input: exactly the senders the round requires at it.
		bool CommitsRemoteInput(uint8_t peerId, uint64_t frame) const;
		/// Records, and on the relay host announces, that the round stops requiring a fenced peer's frames.
		bool WaiveRemoteFrames(uint8_t peerId, uint64_t fromFrame, uint64_t nowMs, bool announce);
		static bool IsFrameWaiver(const NetLockstepStop& stop);
		uint16_t PeerInputDelay(uint8_t peerId) const;
		uint64_t EffectiveStartOf(uint8_t peerId) const;
		/// Whether a reclaimed seat has yet to deliver any input at or past its new effective start.
		bool IsReturningSeatBeforeItsFirstInput(uint8_t peerId) const;
		/// Whether the wait for every peer's published startup has used the round's answer budget.
		bool StartupWaitExpired(uint64_t nowMs) const;
		void TickStartupWait(uint64_t nowMs);
		void FormAgreedFirstFrame(uint64_t nowMs);
		bool SendAgreedStart(uint8_t onlyPeerId = 0);
		void ApplyAgreedStart(const NetLockstepStart& start, uint64_t nowMs);
		uint8_t FirstAliveHumanPeerForTeam(uint8_t team, uint64_t frame) const;
		void Fail(NetLockstepStopReason reason, uint64_t frame, const std::string& message);
		void ScheduleRecoveryStop(NetLockstepStopReason reason, uint64_t frame, const std::string& message);
		void HandleTiming(const NetLockstepTiming& timing, uint64_t nowMs, NetPeerId fromTransport);
		/// Takes a seat the host brought back before this joining round's first frame as a member from that frame.
		void TakeReturnBeforeFirstFrame(const NetLockstepTiming& reclaim);
		void TickTiming(uint64_t nowMs);
		void QueueTiming(const NetLockstepTiming& timing, uint8_t onlyPeer = 0);
		void FlushTimingOutgoing();
		void CommitTiming(uint64_t revision);
		void ApplyTiming(const NetLockstepTiming& timing);
		void PublishCapturePark(uint64_t startFrame);
		void ApplyCapturePark(const NetLockstepTiming& timing);
		uint64_t CaptureParkCapTicks() const;
		/// What the next park's window is sized from: the middle of the last three parks' slowest captures.
		double SteadyCaptureCostMs() const;
		void SendCaptureParkReport(uint64_t nowMs = 0);
		void RetryLateStartReclaims();
		void FlushDeferredParkTimings();
		bool DeclareOverdueInputs(uint64_t frame, uint64_t nowMs, uint64_t firstMissingMs, const std::vector<uint8_t>& missing);
		uint64_t FutureTimingFrame() const;
		struct TimingDecision {
			NetLockstepTiming proposal;
			uint8_t acknowledgedPeers = 0;
			bool committed = false;
			uint64_t proposedAtMs = 0;
		};
		std::map<uint64_t, TimingDecision> m_TimingDecisions;
		/// Whether a decision is committed, applied everywhere it must be and behind the frame the round resumes from.
		bool DecisionSettled(const TimingDecision& decision) const;
		std::vector<std::pair<NetLockstepTiming, NetPeerId>> m_PreStartTiming;
		std::map<uint8_t, std::map<uint64_t, uint16_t>> m_DelayChanges;
		std::map<uint8_t, NetInputDelayEstimator> m_DelayEstimators;
		struct ArrivalLead {
			uint64_t ms = 0; //!< When the input arrived.
			uint64_t frame = 0; //!< The frame it was for.
			uint64_t lead = 0; //!< Frames it arrived ahead of our sim's next tick.
		};
		std::map<uint8_t, std::deque<ArrivalLead>> m_ArrivalLeads; //!< Per remote sender, the recent arrivals of its new input.
		std::map<uint8_t, std::deque<uint32_t>> m_ArrivalLateness; //!< Per remote sender, how long its recent ticks landed after we first missed them.
		static constexpr size_t c_ArrivalLatenessSamples = 64;
		/// The decrease a live delay change may make without a wait at its frame: never more than the sender's inputs arrived early by, less the slow-player bound.
		std::optional<uint16_t> SlackLimitedDecrease(uint8_t peerId, uint16_t proposed, uint16_t current, uint64_t nowMs);
		/// The rise a live delay change makes so the sender's inputs keep the slow-player bound's worth of lead: what the least lead over the last
		/// window lacked, never past the bound above the delay the link's round trip requires.
		std::optional<uint16_t> MarginKeepingIncrease(uint8_t peerId, uint16_t current, uint32_t required, uint64_t nowMs) const;
		/// Whether the blip test lever drops this unreliable frame send.
		bool TestBlipDropsFrameSend(uint64_t targetFrame);
		static constexpr uint64_t c_MarginWindowMs = 1000; //!< The arrivals a rise is judged over: short, so it lands before a spike finds the seat.
		std::map<uint8_t, std::deque<NetLockstepTiming>> m_TimingOutgoing;
		std::map<int64_t, ControllerFrame> m_DeferredControllerFrames;
		uint64_t m_NextTimingRevision = 1;
		uint64_t m_LastTimingSampleMs = UINT64_MAX;
		uint64_t m_LastTimingStatusMs = UINT64_MAX;
		uint64_t m_TimingNowMs = 0;
		std::optional<uint64_t> m_ProductionBaseFrame;
		uint64_t m_ProductionBaseUs = 0;
		uint64_t m_ProductionWaitBaseUs = 0;
		std::map<uint8_t, uint64_t> m_AiHeldSeats;
		std::set<uint8_t> m_ReleasedAiSeats; //!< AI-held seats no returner may reclaim; the AI keeps their units.
		std::set<uint8_t> m_ReleaseWhenHeld; //!< Host: clean leavers whose hold releases the seat as soon as it lands.
		std::map<uint8_t, std::string> m_EvictAfterReclaim; //!< Host: removals that meet a return too close to withdraw; applied once it lands.
		std::optional<NetLockstepStop> m_OwnEndDuringMigration; //!< This peer's own end while its host was being replaced; the new host hears it.
		std::map<uint8_t, NetGameSeatHold> m_HoldTransactions;
		std::map<uint8_t, NetGameSeatReclaim> m_ReclaimTransactions;
		std::optional<uint64_t> m_ConsumerWaitingFrame;
		std::optional<uint64_t> m_FirstMissingFrame;
		uint64_t m_FirstMissingMs = 0;
		std::optional<uint64_t> m_LastDeliveredFrame;
		uint64_t m_ConsumerWaitStartMs = 0;
		uint64_t m_LastTickMs = 0; //!< Our own last Tick; a gap in it is our park, not a peer's silence.
		uint32_t m_LocalStartParkMs = 0; //!< Our own activity restart, as it goes out in our start.
		uint8_t m_LocalDeviceClass = 0; //!< Our own seat device, as it goes out in our start.
		std::array<uint8_t, 16> m_PeerDeviceClasses{}; //!< Host: each seat's device from its latest start.
		bool m_RequirePublishedStart = false;
		bool m_StartWaitAnnounced = false;
		uint64_t m_StartWaitSinceMs = 0;
		bool m_LocalStartupPublished = false;
		std::set<uint8_t> m_PeerStartupPublished; //!< Peers whose startup reading has reached us.
		std::set<uint8_t> m_StartupLinksLost; //!< Host: seats whose link died before the agreed start; the start holds them.
		bool m_AgreedStartApplied = false;
		std::optional<NetLockstepStart> m_AgreedStartRecord;
		std::set<uint8_t> m_StartupHeldSeatStamps; //!< Boundary-held seats stamped on their first committed tick.
		std::map<uint8_t, NetPeerId> m_LateStartReclaims; //!< Boundary-held seats whose late start still owes a reclaim.
		bool m_ConsumerWaitCounted = false;
		bool m_LocalSeatHeld = false;
		uint64_t m_LocalHoldFrame = 0;
		bool m_Playback = false;
		uint32_t m_LocalSeatReclaims = 0;

		INetTransport* m_Transport = nullptr;
		NetLockstepConfig m_Config;
		NetMatchConfig m_OpeningMatchConfig;
		NetHash32 m_RoundConfigHash{};
		NetLockstepState m_State = NetLockstepState::Idle;
		NetLockstepStats m_Stats;
		std::vector<uint8_t> m_RemotePeerIds; //!< Every peer except local; derived at Start.
		std::map<uint8_t, NetPeerId> m_RemoteTransports; //!< Lockstep peerId -> transport id for each remote.
		std::set<uint8_t> m_RemoteStartsReceived; //!< Remotes whose matching Start we've accepted; run when all present.
		std::map<uint8_t, NetLockstepStart> m_RemoteStarts; //!< Each accepted start, re-sent when a peer repeats its own.
		std::set<uint8_t> m_PeersPlayedThisRound; //!< Remotes whose frames this round took; they are not still forming it.
		std::map<uint8_t, uint64_t> m_PeerLeaveFrames; //!< Cleanly-left peers -> the first frame WITHOUT their data.
		std::map<uint8_t, uint64_t> m_PeerFrameWaivers; //!< Fenced peers -> the first frame the round stopped requiring.
		std::set<uint8_t> m_LeftSeatsHeld;  //!< Left peers whose seat is still reclaimable, resolved once a tick.
		std::set<uint8_t> m_DroppedSeats;   //!< Classic holds pause; bounded holds commit empty input from their agreed frame.
		std::set<uint8_t> m_LeavesHeardAhead; //!< Classic leavers whose leave frame was past the round's next frame when heard.
		std::map<uint8_t, NetLockstepHoldResolution> m_DroppedSeatResolutions;
		std::map<uint8_t, uint64_t> m_DroppedAtMs;
		uint64_t m_LastHoldHeartbeatMs = 0;
		std::optional<NetLockstepSeatSnapshot> m_SeatSnapshot;
		bool m_SeatSnapshotUnread = false;
		std::set<uint8_t> m_PendingSeatSnapshotPeers;
		std::map<uint8_t, uint64_t> m_PeerLastHeardMs; //!< peerId -> when its last packet arrived; the host's drop clock.
		std::set<uint8_t> m_UnreachablePeers; //!< Remotes whose forwards never landed, dropped on the next tick.
		std::set<uint8_t> m_CongestedPeers; //!< Remotes whose last refusal was our own full queue.
		std::set<uint8_t> m_HeldForCongestion; //!< Congestion episodes already reported, so the hold is logged once.
		std::map<uint8_t, std::deque<std::vector<uint8_t>>> m_RelayBacklog; //!< peerId -> forwards the transport refused, awaiting retry.
		std::map<uint8_t, std::set<uint64_t>> m_RelayedTicks; //!< Relay host: sender -> the ticks already sent on to the others.
		std::map<uint8_t, std::map<uint64_t, NetLockstepFrame>> m_RelayedTickFrames; //!< Relay host: sender -> those ticks, whole, for a resend.
		std::map<uint8_t, uint64_t> m_ReliableFramesThrough; //!< A member catching up reads frames on the reliable lane through this tick.
		std::map<uint8_t, uint64_t> m_RelayBacklogSinceMs; //!< peerId -> when its backlog stopped draining.
		std::map<uint8_t, uint64_t> m_PeerEffectiveStart; //!< peerId -> the first frame that carries this sender's input.
		struct PeerAdmission { uint64_t frame; uint16_t delay; };
		std::map<uint8_t, PeerAdmission> m_PeerAdmissions;
		uint64_t m_LastQueuedTargetFrame = UINT64_MAX; //!< Highest produced target frame; UINT64_MAX until the first queue.
		std::set<uint64_t> m_ObservationEpochs;        //!< Every announced frame senders spell their keys out from again.
		std::set<uint64_t> m_HostAcceptedLocalFrames;
		std::map<uint8_t, uint64_t> m_ObservationEpochApplied; //!< sender -> the newest epoch its encode table was reset at.
		size_t m_LastAdmissionReplayFrames = 0;        //!< What the last admission replayed, for the report.
		std::function<void(const NetTransportEvent&)> m_SessionEventSink; //!< Forwards session traffic (reconnect handshakes) mid-match.
		NetLockstepSeatState (*m_SeatStateSource)(void*, uint8_t, NetPeerId) = nullptr;
		void* m_SeatStateContext = nullptr;
		std::string m_LastLeaveMessage; //!< The message the round ends with once no left seat is held any more.
		bool m_RelayHost = false; //!< Host-star relay: forward each remote's frames/checksums to the other remotes.
		bool m_DeferStops = false;
		bool m_ResyncPrimed = false;
		bool m_ResumeAdmissionPending = false;
		uint64_t m_SynchronizedCaptureStartFrame = UINT64_MAX;
		uint64_t m_SynchronizedCaptureEndFrame = 0;
		double m_SynchronizedCaptureBudgetMs = 250.0;
		uint64_t m_CaptureParkRevision = 0;
		uint64_t m_CaptureParkDeadlineMs = 0;
		uint64_t m_CaptureParkPublishedEndFrame = UINT64_MAX;
		uint64_t m_HighestParkEndFrame = 0;
		uint32_t m_PendingCaptureReportMs = 0;
		uint64_t m_PendingCaptureTick = UINT64_MAX; //!< The tick the pending capture report measured.
		uint64_t m_ReportedCaptureParkStart = UINT64_MAX; //!< The park this peer's last capture report went to.
		uint32_t m_ReportedCaptureParkMs = 0;
		uint64_t m_CaptureReportSentMs = 0;
		bool m_CaptureReportResent = false;
		std::map<uint8_t, uint32_t> m_CaptureParkReportsMs;
		std::deque<uint32_t> m_ParkCaptureHistoryMs; //!< Host: the slowest capture of each recent park, newest last.
		std::map<uint8_t, std::pair<uint64_t, uint64_t>> m_ResendRequests; //!< Sender -> (the tick last asked for, when).
		uint64_t m_MissingSinceFrame = UINT64_MAX; //!< The committed frame this peer has waited on, for the resend request.
		std::map<uint64_t, uint64_t> m_DecisionCommittedAtMs; //!< Host: a timing decision's frame -> when this host committed it.
		uint64_t m_MissingSinceMs = 0;
		uint64_t m_DescribedWaitFrame = UINT64_MAX; //!< The waited frame whose long wait was already named.
		uint64_t m_LastProducedFrame = UINT64_MAX; //!< The produced frame of this peer's last queued local input.
		uint64_t m_FinalFrame = UINT64_MAX;
		bool m_GoodbyeDrain = false;
		std::map<uint64_t, uint64_t> m_CommittedAtMs; //!< Host: when each recent frame was committed, the moment a seat could first act on it.
		uint64_t m_ParkFrameSimulated = UINT64_MAX; //!< The last park frame this peer simulated.
		uint64_t m_ParkFrameSimulatedMs = 0; //!< When it did: a seat's first post-park input is due a delay after the park's last frame.
		std::vector<NetLockstepTiming> m_DeferredParkTimings;
		std::map<uint64_t, std::vector<NetGameCommand>> m_ParkCarriedCommands; //!< This peer's commands a park emptied, by the frame they targeted; they ride its next input.
		bool m_ApplyingDeferredParkTiming = false;
		bool m_CaptureParkAwaitingReports = false;
		bool m_CaptureParkFinalized = false;
		std::optional<NetLockstepStop> m_PendingRecoveryStop;
		std::optional<NetLockstepStop> m_PendingCompleteStop;
		std::optional<uint64_t> m_LastCompletedSimulationTick;
		uint64_t m_WaitingFrame = 0;
		uint64_t m_WaitStartMs = 0;
		uint64_t m_AuthorityLastHeardMs = 0;
		std::deque<uint32_t> m_AuthorityGaps; //!< Client: the recent gaps between the host's packets while it played.
		static constexpr size_t c_AuthorityGapSamples = 256;
		uint32_t m_AuthorityLongestGapMs = 0; //!< Client: the longest gap the host left while it played this round.
		uint64_t m_LastLivenessMs = 0; //!< Host: when it last told its clients it is alive while its round waited.
		uint64_t m_OwnFramesSent = 0; //!< Frames of its own this peer has sent.
		uint64_t m_LivenessFramesSeen = 0; //!< Host: the count its liveness last saw move.
		uint64_t m_LivenessQuietSinceMs = 0; //!< Host: since when it has sent no frame of its own.
		uint32_t m_AnnouncedCaptureEvery = 0; //!< Period of the captures every peer takes; 0 when there are none.
		bool m_AwaitingReplayedSeatState = false; //!< A round joined from a replayed tail: no commit until that tail's seat changes are taken.
		std::map<uint8_t, std::pair<uint64_t, uint64_t>> m_RetiredReclaimGaps; //!< A return's neutral gap that still covers frames before the hold that ended it.
		std::set<uint64_t> m_AnnouncedCaptureTicks; //!< Named captures every peer takes at the end of these ticks.
		uint64_t m_LastStallFrame = UINT64_MAX;
		std::map<uint64_t, std::vector<ControllerFrame>> m_LocalFrames;
		std::map<uint64_t, std::map<uint8_t, std::vector<ControllerFrame>>> m_RemoteFrames; //!< frame -> (peerId -> frames)
		std::map<uint64_t, std::vector<NetGameCommand>> m_LocalCommands;
		std::map<uint64_t, std::map<uint8_t, std::vector<NetGameCommand>>> m_RemoteCommands; //!< frame -> (peerId -> commands)
		std::map<uint64_t, std::vector<NetSoundObservation>> m_LocalObservations;
		std::map<uint64_t, std::map<uint8_t, std::vector<NetSoundObservation>>> m_RemoteObservations; //!< frame -> (peerId -> observations)
		std::map<uint64_t, std::vector<NetValueObservation>> m_LocalValueObservations;
		std::map<uint64_t, std::map<uint8_t, std::vector<NetValueObservation>>> m_RemoteValueObservations;
		uint64_t m_RoundId = 0;
		uint64_t m_LastStartSentMs = UINT64_MAX;
		std::map<uint8_t, uint64_t> m_LastStartAnswerMs; //!< peerId -> when we last answered its repeated start.
		std::map<uint64_t, NetLockstepFrame> m_ResendFrames; //!< The frames a followed round still owes the host, whole.
		struct RecoveryOutgoing {
			NetLockstepFrame frame;
			std::vector<uint8_t> bytes;
			std::map<uint8_t, size_t> nextOffsets;
			bool commitLocal = false;
		};
		struct RecoveryIncoming {
			uint64_t targetFrame = 0;
			uint32_t totalBytes = 0;
			std::vector<uint8_t> bytes;
		};
		std::deque<RecoveryOutgoing> m_RecoveryOutgoing;
		std::map<uint8_t, RecoveryIncoming> m_RecoveryIncoming;
		std::map<uint8_t, uint64_t> m_RecoveryBlockedSinceMs;
		std::map<uint64_t, NetLockstepFrame> m_LocalInputHistory;
		std::set<uint8_t> m_RemoteFrameWindow; //!< Remotes whose Ack advertised the frame-window capability.
		std::vector<std::vector<uint8_t>> m_ResyncPrimeInputs;
		std::set<std::pair<uint64_t, uint8_t>> m_InstalledResyncTargets;
		NetSoundObservationTables m_ObservationDecodeTables; //!< One slot table per sender this peer decodes, for this round only.
		// What this peer spells its own observations with, and what a relay host re-encodes each other
		// sender's with. A relay table is fed by exactly the frames it forwards, which is exactly what its
		// sender encoded, so a forward is the same size as the packet it came from and its receivers see
		// every binding the sender made.
		NetSoundObservationTables m_ObservationEncodeTables;
		/// Per sender, the observation bytes of the ticks a window may still repeat. Reset with the tables.
		std::map<uint8_t, NetLockstepObservationBlocks> m_ObservationBlocks;
		std::vector<NetSoundObservation> m_PendingObservations; //!< What the last frame could not hold; rides the next one.
		std::vector<NetSoundObservation> m_DroppedObservations; //!< Readings the wire never carried, for their sampler to take back.
		std::vector<NetValueObservation> m_PendingValueObservations;
		std::vector<NetValueObservation> m_DroppedValueObservations;
		std::map<uint8_t, std::deque<NetLockstepFrame>> m_PreStartFrames; //!< A peer's frames that outran its start.
		std::map<uint8_t, std::deque<NetLockstepChecksum>> m_PreStartChecksums;
		std::map<uint64_t, std::array<uint8_t, 32>> m_LocalChecksums;
		std::map<uint8_t, uint64_t> m_AuthoritativeCommandAcks;
		std::map<uint64_t, std::map<uint8_t, std::array<uint8_t, 32>>> m_RemoteChecksums; //!< frame -> (peerId -> hash)
		std::deque<NetLockstepReadyFrame> m_ReadyFrames;
		std::map<uint64_t, NetLockstepReadyFrame> m_ReadyHistory;

		bool AllRemoteStartsReceived() const { return m_RemoteStartsReceived.size() == m_RemotePeerIds.size(); }
		bool IsKnownRemotePeer(uint8_t peerId) const;
		void RelayToOtherRemotes(const NetLockstepPacket& packet, uint8_t fromPeerId);
		/// Relay host on the unreliable lane: sends each tick of an arrived packet on once, oldest first, with the ticks before it.
		void RelayArrivedTicks(const NetLockstepFrame& frame);
		/// The lane a packet takes to one peer: frames ride the frame lane unless that peer is still catching up.
		NetTransportLane LaneTo(uint8_t peerId, const NetLockstepPacket& packet, NetTransportLane lane) const;
	};

} // namespace RTE
