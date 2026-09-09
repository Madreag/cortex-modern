#pragma once

#include "ControllerFrame.h"
#include "NetGameCommand.h"
#include "NetMatchConfig.h"
#include "NetTransport.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace RTE {

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

		bool operator==(const NetLockstepFrame& rhs) const;
	};

	struct NetLockstepAck {
		uint8_t senderPeerId = 0;
		uint64_t highestContiguousFrame = 0;
		uint32_t receivedMask = 0;

		bool operator==(const NetLockstepAck&) const = default;
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

		bool operator==(const NetLockstepChecksum&) const = default;
	};

	using NetLockstepPayload = std::variant<NetLockstepStart, NetLockstepFrame, NetLockstepAck, NetLockstepStop, NetLockstepChecksum>;

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
	};

	struct NetLockstepReadyFrame {
		uint64_t frame = 0;
		std::vector<ControllerFrame> localFrames;
		std::vector<ControllerFrame> remoteFrames;
		std::vector<NetGameCommand> localCommands;
		std::vector<NetGameCommand> remoteCommands;
		std::vector<NetSoundObservation> localObservations;
		std::vector<NetSoundObservation> remoteObservations;
	};

	/// One remote's share of the round, enough to tell a peer that stopped SENDING from one the host
	/// stopped RELAYING to, and from one whose frames arrived and were refused.
	struct NetLockstepPeerStats {
		uint32_t framePacketsReceived = 0;
		uint64_t controllerFramesReceived = 0;
		uint32_t framesContributed = 0; //!< This peer's frames that reached a committed tick.
		uint32_t duplicateFrames = 0;
		uint32_t outOfOrderFrames = 0;
		uint32_t futureFrameDrops = 0;
		uint32_t staleRoundPackets = 0;
		uint32_t preStartBuffered = 0;
		uint32_t relayPacketsSent = 0; //!< Host: packets forwarded TO this peer.
		uint32_t relaySendFailures = 0; //!< Host: forwards the transport refused for this peer.
		uint32_t relayResends = 0; //!< Host: refused forwards a later retry did deliver.
		uint64_t relayBytesSent = 0; //!< Host: encoded bytes forwarded to this peer, the send-buffer pressure it sees.
		uint32_t largestRelayPacketBytes = 0; //!< Host: the biggest single forward, so an oversized frame is visible.
		uint32_t relayBacklogPackets = 0; //!< Host: forwards still held for this peer.
		uint64_t highestTargetFrame = 0;
		uint64_t lastHeardMs = 0;
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
		uint64_t localControllerFramesSent = 0;
		uint64_t remoteControllerFramesReceived = 0;
		uint64_t remoteControllerFramesAccepted = 0;
		uint32_t framesAccepted = 0;
		uint32_t duplicateFrames = 0;
		uint32_t outOfOrderFrames = 0;
		uint32_t futureFrameDrops = 0; //!< Frames beyond the skew window, dropped so the maps stay bounded.
		uint32_t missingFrameStalls = 0;
		uint32_t relayPacketsSent = 0; //!< Host-star: forwards this peer made on behalf of another.
		uint32_t relaySendFailures = 0; //!< Forwards the transport refused; on a reliable lane the receiver never recovers them.
		uint32_t relayResends = 0; //!< Refused forwards a later retry did deliver.
		uint64_t relayBytesSent = 0; //!< Encoded bytes this host forwarded, across every peer.
		uint32_t largestRelayPacketBytes = 0;
		uint64_t relayBacklogBytes = 0; //!< Bytes still held for peers whose forwards were refused.
		uint64_t observationsCarried = 0; //!< Times a full frame left a reading for the next one to carry.
		uint64_t observationsDropped = 0; //!< Carried readings dropped because new sounds outran the wire for frames on end.
		uint32_t unresolvedObservationPackets = 0; //!< Frames dropped because an observation named a slot this peer never got.
		uint32_t relayObservationOverflows = 0; //!< Forwards that could not carry a frame's whole observation set; the tables would disagree.
		uint32_t peersDroppedSilent = 0; //!< Remotes the host adjudicated gone for going quiet, not for closing their socket.
		uint32_t timeouts = 0;
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
		static constexpr uint16_t c_Version = 15;
		// Versions 8 and 9 have the same layout minus the AIEquip and AIOrder commands; recordings made under them still decode.
		// Version 11 adds the round tag to starts, frames and checksums, and sound observations to frames.
		// Version 12 adds the system-authored Reseat command.
		// Version 14 spells a sound observation's key once per sender and refers to it by slot after that.
		// Version 15 says how many keys the sender had spelled out before the packet, so a receiver that
		// missed one refuses instead of reading a reused slot as the key it held before.
		static constexpr uint16_t c_MinVersion = 8;
		static constexpr uint16_t c_RoundVersion = 11;
		static constexpr uint16_t c_ObservationSlotVersion = 14;
		static constexpr uint16_t c_ObservationBindingSequenceVersion = 15;
		static constexpr size_t c_MaxObservationsPerPacket = NetSoundObservationDictionary::c_MaxSlots;
		// What one frame's observations may cost. The compact form makes 4096 of them about 21 KB, so a
		// frame that hits this is carrying keys nobody has seen before; the rest ride the next frame.
		static constexpr size_t c_MaxObservationBytesPerPacket = 24U * 1024U;
		// How much a sender may hold back for later. Reaching this needs thousands of sounds nobody has
		// heard before, every frame, for frames on end; past it the stalest readings go.
		static constexpr size_t c_MaxCarriedObservations = NetSoundObservationDictionary::c_MaxSlots;
		static constexpr uint16_t c_HeaderBytes = 16;
		static constexpr size_t c_MaxPayloadBytes = 64U * 1024U;
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

		static NetLockstepPacketType PacketTypeOf(const NetLockstepPayload& payload);
		static const char* PacketTypeName(NetLockstepPacketType type);
		static const char* StopReasonName(NetLockstepStopReason reason);
		static const char* ErrorCodeName(NetLockstepErrorCode code);

		/// Without a dictionary every observation spells out its key, so the packet stands alone; that is
		/// what a replay record and a one-shot round trip want. With one, only what fits the observation
		/// byte budget is encoded and outObservationsEncoded says how many, so the caller can carry the
		/// rest; the dictionary is touched only once the packet is certain to encode.
		static bool Encode(const NetLockstepPacket& packet, std::vector<uint8_t>& outBytes, NetLockstepError* error = nullptr, NetSoundObservationDictionary* dictionary = nullptr, size_t* outObservationsEncoded = nullptr);
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
		/// Feeds one recorded tick straight into the commit path: command senders preserved, no
		/// delay math, no wire — the replay's committed frame is exactly the recording's.
		bool QueueReplayFrame(uint64_t frame, std::vector<ControllerFrame> frames, std::vector<NetGameCommand> commands, std::string* error = nullptr, std::vector<NetSoundObservation> observations = {});
		/// Rewinds a playback coordinator to re-commit from an earlier frame (the rollback
		/// fidelity gate re-runs a window). Replay mode only — there is no wire to rewind.
		bool RewindReplay(uint64_t firstFrame, std::string* error = nullptr);
		bool QueueLocalInput(uint64_t producedFrame, const std::vector<ControllerFrame>& frames, const std::vector<NetGameCommand>& commands, std::string* error = nullptr, const std::vector<NetSoundObservation>& observations = {});
		bool SubmitLocalChecksum(uint64_t frame, const std::array<uint8_t, 32>& hash, std::string* error = nullptr);
		void Tick(uint64_t nowMs);
		void Complete(const std::string& message = "complete");
		/// Announces a clean local leave: peers keep our frames through the last produced one, then
		/// advance without us. The relay host cannot leave a 3+ match alive (it is the star's hub),
		/// so a host leave completes the match for everyone instead.
		void Leave(const std::string& message = "player left");
		/// Ends the round on every peer so the match reconvenes and reloads the host's snapshot
		/// (a rejoin or an operator-forced heal). Host-initiated.
		void RequestResync(const std::string& message = "resync requested");
		/// Keeps recovery and completion aligned with applied simulation ticks, while input may be prefetched.
		void DeferStopsToTickBoundary() { m_DeferStops = true; }
		bool HasPendingRecoveryStop() const { return m_PendingRecoveryStop.has_value(); }
		bool FinishSimulationTick(uint64_t completedTick);
		/// Receives the session-protocol traffic (a reconnecting peer's handshake) the coordinator
		/// would otherwise discard while it owns the transport queue.
		void SetSessionEventSink(std::function<void(const NetTransportEvent&)> sink) { m_SessionEventSink = std::move(sink); }
		/// The H4 seat state, asked for by lockstep peer id and (on a disconnect) the transport that
		/// went away. Without one every seat reads as neither fenced nor held, which is the pre-H4 round.
		void SetSeatStateSource(NetLockstepSeatState (*source)(void*, uint8_t, NetPeerId), void* context);
		bool PopReadyFrame(NetLockstepReadyFrame& outFrame);
		/// The local frames already queued for a future frame; the local-actor preview runs them early.
		bool PeekLocalFrames(uint64_t frame, std::vector<ControllerFrame>& outFrames) const;

		NetLockstepState GetState() const { return m_State; }
		bool IsRunning() const { return m_State == NetLockstepState::Running; }
		bool IsFailed() const { return m_State == NetLockstepState::Failed; }
		bool IsStopped() const { return m_State == NetLockstepState::Stopped; }
		const NetLockstepStats& GetStats() const { return m_Stats; }
		/// The readings this peer held and then had to drop. Their sampler must forget it ever sent them,
		/// or it will not offer them again until the sound's audibility moves.
		std::vector<NetSoundObservation> TakeDroppedObservations();
		const NetLockstepConfig& GetConfig() const { return m_Config; }
		/// The round every accepted packet carries; 0 on a client until the host's start arrives.
		uint64_t GetRoundId() const { return m_RoundId; }
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
		/// Whether the round is only still alive because a dropped seat may still be reclaimed: every
		/// remote has left and at least one of their seats is inside its window. Nobody can disagree
		/// with this peer about it, because while it holds there is no other peer in the round.
		bool IsHoldingSeatForReclaim() const;
		// P2's 20 000 ms reclaim window as a count of frames at the pinned timestep (c_DefaultDeltaTimeS
		// = 0.0166666 s, so 20 000 / 16.6666 = 1200). A frame, never a clock: every peer must reach the
		// same answer at the same tick, and only the tick is shared.
		static constexpr uint64_t c_ReclaimHoldFrames = 1200;
		/// Whether a dropped seat is still inside its reclaim window as of the given frame. Derived from
		/// the relayed leave notice alone, so every peer in the round answers identically at the same
		/// tick - the question above is about a round with nobody left and is answered host-side.
		bool IsSeatHeldForReclaimAtFrame(uint64_t frame) const;
		/// Whether the round has yet to commit a frame. A resync relaunch lands here: the ledgered
		/// reseat rides the first committed frame, so nothing the round produced can be judged before it.
		bool HasCommittedAFrame() const { return m_Stats.framesAccepted > 0; }
		/// Whether this relay host still owes a peer a forward it has not managed to send. The star's
		/// hub cannot leave while this is true: a client waiting on that frame loses the round.
		bool HasPendingRelayWork() const { return m_RelayHost && !m_RelayBacklog.empty(); }
		/// Names the required peers the next frame still waits on; empty when none are missing.
		std::string DescribeMissingPeers() const;
		/// The peer's roster display name, or "peer N" when the roster has none.
		std::string DescribePeer(uint8_t peerId) const;
		std::string BuildReportJson() const;

		static const char* StateName(NetLockstepState state);

	private:
		/// Sends to every remote, or to one when onlyPeerId names it.
		bool SendPacket(const NetLockstepPacket& packet, NetTransportLane lane, std::string* error = nullptr, NetSoundObservationDictionary* dictionary = nullptr, size_t* outObservationsEncoded = nullptr, uint8_t onlyPeerId = 0);
		void HandleEvent(const NetTransportEvent& event, uint64_t nowMs);
		void HandlePacket(const NetLockstepPacket& packet, uint64_t nowMs, NetPeerId fromTransport);
		void HandleStart(const NetLockstepStart& start, uint64_t nowMs, NetPeerId fromTransport);
		void HandleFrame(const NetLockstepFrame& frame, uint64_t nowMs, NetPeerId fromTransport, bool relay = true);
		uint8_t LockstepPeerOfTransport(NetPeerId transportPeerId) const;
		bool SendStart(std::string* error, uint8_t onlyPeerId = 0);
		/// Sends a peer that repeated its start what it needs to form the round.
		void AnswerRepeatedStart(uint8_t peerId, uint64_t nowMs);
		/// Whether we take our round from that sender: the peer we are connected to. Everyone else's
		/// start reaches us relayed, carrying the round its sender adopted rather than the host's.
		bool IsRoundAuthority(uint8_t peerId, NetPeerId fromTransport) const;
		/// Whether a start describes the round this peer is configured for, field by field.
		bool StartMatchesConfig(const NetLockstepStart& start) const;
		/// Leaves the round we formed for the one the host is in, keeping our own production.
		void ReadoptRound(uint64_t roundId, uint64_t nowMs);
		/// Delivers the frames and checksums a peer sent before its start reached us.
		void FlushPreStart(uint8_t peerId, uint64_t nowMs);
		/// Clears everything one round owns, so leaving a round cannot carry a fact from it.
		void ResetRoundState();
		/// Sends the production a followed round owes the host, in order, retrying a refused send.
		void FlushResendFrames();
		void HandleStop(const NetLockstepStop& stop, uint64_t nowMs, NetPeerId fromTransport);
		void HandleChecksum(const NetLockstepChecksum& checksum, NetPeerId fromTransport);
		/// Whether a packet's claimed sender owns the transport it arrived on. Only the relay host
		/// receives each remote directly; clients get everything via the relay and trust the host.
		bool SenderOwnsTransport(uint8_t claimedPeerId, NetPeerId fromTransport) const;
		void CompareChecksums(uint64_t frame);
		void AdvanceReadyFrames(uint64_t nowMs);
		void ApplyPeerLeave(uint8_t peerId, uint64_t firstFrameWithout, const std::string& message, uint64_t nowMs, bool announced);
		/// The first frame this peer has no data for, walking up from the committed one.
		uint64_t FirstFrameWithout(uint8_t peerId) const;
		/// How long the host lets a required remote go quiet before calling it gone. Half the
		/// missing-frame grace, so the relayed notice still has the other half to reach the survivors.
		uint64_t PeerSilenceLeaveMs() const { return m_Config.timeoutMs / 2; }
		/// Relay host: a required remote that has blocked the round this long has left, whatever its
		/// socket still says. Waiting for the transport means waiting on the dead peer's own process.
		void AdjudicateSilentPeers(uint64_t nowMs);
		void CountRelaySent(NetLockstepPeerStats& peerStats, size_t bytes);
		uint64_t RelayBacklogBytes() const;
		void UpdateRelayBacklogBytes() { m_Stats.relayBacklogBytes = RelayBacklogBytes(); }
		uint32_t RelayBacklogPackets(uint8_t peerId) const;
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
		/// Ends a round every remote has left once the last held seat's reclaim window has closed.
		void EndRoundIfNobodyIsComingBack();
		bool IsRemoteRequiredForFrame(uint8_t peerId, uint64_t frame) const;
		uint16_t PeerInputDelay(uint8_t peerId) const;
		uint64_t EffectiveStartOf(uint8_t peerId) const;
		uint8_t FirstAliveHumanPeerForTeam(uint8_t team, uint64_t frame) const;
		void Fail(NetLockstepStopReason reason, uint64_t frame, const std::string& message);
		void ScheduleRecoveryStop(NetLockstepStopReason reason, uint64_t frame, const std::string& message);

		INetTransport* m_Transport = nullptr;
		NetLockstepConfig m_Config;
		NetLockstepState m_State = NetLockstepState::Idle;
		NetLockstepStats m_Stats;
		std::vector<uint8_t> m_RemotePeerIds; //!< Every peer except local; derived at Start.
		std::map<uint8_t, NetPeerId> m_RemoteTransports; //!< Lockstep peerId -> transport id for each remote.
		std::set<uint8_t> m_RemoteStartsReceived; //!< Remotes whose matching Start we've accepted; run when all present.
		std::map<uint8_t, NetLockstepStart> m_RemoteStarts; //!< Each accepted start, re-sent when a peer repeats its own.
		std::set<uint8_t> m_PeersPlayedThisRound; //!< Remotes whose frames this round took; they are not still forming it.
		std::map<uint8_t, uint64_t> m_PeerLeaveFrames; //!< Cleanly-left peers -> the first frame WITHOUT their data.
		std::set<uint8_t> m_LeftSeatsHeld;  //!< Left peers whose seat is still reclaimable, resolved once a tick.
		std::set<uint8_t> m_DroppedSeats;   //!< Left peers whose transport died rather than announcing; carried by the leave notice, so every peer has it.
		std::map<uint8_t, uint64_t> m_PeerLastHeardMs; //!< peerId -> when its last packet arrived; the host's drop clock.
		std::set<uint8_t> m_UnreachablePeers; //!< Remotes whose forwards never landed, dropped on the next tick.
		std::map<uint8_t, std::deque<std::vector<uint8_t>>> m_RelayBacklog; //!< peerId -> forwards the transport refused, awaiting retry.
		std::map<uint8_t, uint64_t> m_RelayBacklogSinceMs; //!< peerId -> when its backlog stopped draining.
		std::map<uint8_t, uint64_t> m_PeerEffectiveStart; //!< peerId -> the first frame that carries this sender's input.
		uint64_t m_LastQueuedTargetFrame = UINT64_MAX; //!< Highest produced target frame; UINT64_MAX until the first queue.
		std::function<void(const NetTransportEvent&)> m_SessionEventSink; //!< Forwards session traffic (reconnect handshakes) mid-match.
		NetLockstepSeatState (*m_SeatStateSource)(void*, uint8_t, NetPeerId) = nullptr;
		void* m_SeatStateContext = nullptr;
		std::string m_LastLeaveMessage; //!< The message the round ends with once no left seat is held any more.
		bool m_RelayHost = false; //!< Host-star relay: forward each remote's frames/checksums to the other remotes.
		bool m_DeferStops = false;
		std::optional<NetLockstepStop> m_PendingRecoveryStop;
		std::optional<NetLockstepStop> m_PendingCompleteStop;
		std::optional<uint64_t> m_LastCompletedSimulationTick;
		uint64_t m_WaitingFrame = 0;
		uint64_t m_WaitStartMs = 0;
		uint64_t m_LastStallFrame = UINT64_MAX;
		std::map<uint64_t, std::vector<ControllerFrame>> m_LocalFrames;
		std::map<uint64_t, std::map<uint8_t, std::vector<ControllerFrame>>> m_RemoteFrames; //!< frame -> (peerId -> frames)
		std::map<uint64_t, std::vector<NetGameCommand>> m_LocalCommands;
		std::map<uint64_t, std::map<uint8_t, std::vector<NetGameCommand>>> m_RemoteCommands; //!< frame -> (peerId -> commands)
		std::map<uint64_t, std::vector<NetSoundObservation>> m_LocalObservations;
		std::map<uint64_t, std::map<uint8_t, std::vector<NetSoundObservation>>> m_RemoteObservations; //!< frame -> (peerId -> observations)
		uint64_t m_RoundId = 0;
		uint64_t m_LastStartSentMs = UINT64_MAX;
		std::map<uint8_t, uint64_t> m_LastStartAnswerMs; //!< peerId -> when we last answered its repeated start.
		std::set<uint64_t> m_ResendFrames; //!< Target frames a followed round still owes the host.
		NetSoundObservationTables m_ObservationDecodeTables; //!< One slot table per sender this peer decodes, for this round only.
		// What this peer spells its own observations with, and what a relay host re-encodes each other
		// sender's with. A relay table is fed by exactly the frames it forwards, which is exactly what its
		// sender encoded, so a forward is the same size as the packet it came from and its receivers see
		// every binding the sender made.
		NetSoundObservationTables m_ObservationEncodeTables;
		std::vector<NetSoundObservation> m_PendingObservations; //!< What the last frame could not hold; rides the next one.
		std::vector<NetSoundObservation> m_DroppedObservations; //!< Readings the wire never carried, for their sampler to take back.
		std::map<uint8_t, std::deque<NetLockstepFrame>> m_PreStartFrames; //!< A peer's frames that outran its start.
		std::map<uint8_t, std::deque<NetLockstepChecksum>> m_PreStartChecksums;
		std::map<uint64_t, std::array<uint8_t, 32>> m_LocalChecksums;
		std::map<uint64_t, std::map<uint8_t, std::array<uint8_t, 32>>> m_RemoteChecksums; //!< frame -> (peerId -> hash)
		std::deque<NetLockstepReadyFrame> m_ReadyFrames;

		bool AllRemoteStartsReceived() const { return m_RemoteStartsReceived.size() == m_RemotePeerIds.size(); }
		bool IsKnownRemotePeer(uint8_t peerId) const;
		void RelayToOtherRemotes(const NetLockstepPacket& packet, uint8_t fromPeerId);
	};

} // namespace RTE
