#include "NetLockstepSelfTest.h"

#include "LoopbackTransport.h"
#include "NetLockstep.h"
#include "NetMatchReplay.h"
#include "NetProtocol.h"
#include "NetReconnectLedger.h"
#include "NetReconnectUx.h"
#include "NetResyncSelfTest.h"
#include "NetResyncRuntimeSelfTest.h"
#include "System/ScenarioRunner.h"
#include "ActivityMan.h"
#include "AudioMan.h"
#include "MovableMan.h"
#include "MovableObject.h"
#include "TimerMan.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>

namespace RTE {

	namespace {
		bool TestSnapshotConstructionKeepsPendingCommands(std::string* error) {
			const NetGameCommand pending{2, NetGameSwitchControl{4242, 0, 2}};
			const NetGameCommand temporary{2, NetGameSwitchControl{7777, 0, 2}};
			ScenarioRunner::DrainLocalGameCommands();
			ScenarioRunner::EnqueueLocalGameCommand(pending);
			g_MovableMan.SetRestoringSnapshot(true);
			ScenarioRunner::EnqueueLocalGameCommand(temporary);
			g_MovableMan.SetRestoringSnapshot(false);
			if (ScenarioRunner::DrainLocalGameCommands() != std::vector<NetGameCommand>{pending}) {
				*error = "snapshot construction changed the genuine pending command queue";
				return false;
			}
			return true;
		}

		ControllerFrame MakeFrame(int64_t actorId, uint64_t stateMask) {
			ControllerFrame frame;
			frame.actorUniqueID = actorId;
			frame.stateMask = stateMask;
			frame.analogMoveX = 123;
			frame.analogMoveY = -456;
			frame.analogAimX = 789;
			frame.analogAimY = -321;
			frame.inputMode = static_cast<uint8_t>(Controller::CIM_PLAYER);
			frame.playerRaw = Players::PlayerOne;
			frame.SetQuickDisabled(true);
			frame.aimAngle = 0.125F;
			frame.viewPointX = 12.5F;
			frame.viewPointY = -34.25F;
			frame.equippedFGUniqueID = 9001;
			frame.fgHandPosX = 1.5F;
			frame.fgHandPosY = 2.5F;
			return frame;
		}

		bool EncodePacket(const NetLockstepPacket& packet, std::vector<uint8_t>& bytes, std::string* error) {
			NetLockstepError encodeError;
			if (!NetLockstepCodec::Encode(packet, bytes, &encodeError)) {
				*error = "encode failed: " + encodeError.message;
				return false;
			}
			return true;
		}

		bool RoundTrip(const NetLockstepPacket& packet, std::string* error) {
			std::vector<uint8_t> bytes;
			if (!EncodePacket(packet, bytes, error)) {
				return false;
			}
			const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(bytes);
			if (!decoded.ok) {
				*error = "decode failed: " + decoded.error.message;
				return false;
			}
			if (!(decoded.packet == packet)) {
				*error = "decoded packet differed for " + std::string(NetLockstepCodec::PacketTypeName(NetLockstepCodec::PacketTypeOf(packet.payload)));
				return false;
			}
			return true;
		}

		bool ExpectDecodeError(const std::vector<uint8_t>& bytes, NetLockstepErrorCode code, std::string* error) {
			const NetLockstepDecodeResult result = NetLockstepCodec::Decode(bytes);
			if (result.ok) {
				*error = "expected decode failure for " + std::string(NetLockstepCodec::ErrorCodeName(code));
				return false;
			}
			if (result.error.code != code) {
				*error = "expected " + std::string(NetLockstepCodec::ErrorCodeName(code)) +
				         " got " + NetLockstepCodec::ErrorCodeName(result.error.code) +
				         ": " + result.error.message;
				return false;
			}
			return true;
		}

		NetSoundObservation MakeObservation(uint8_t sender, uint64_t objectUID, uint64_t tick, uint64_t phase, uint64_t ordinal, float value) {
			NetSoundObservation observation;
			observation.senderPeerId = sender;
			observation.objectUID = objectUID;
			observation.tick = tick;
			observation.phase = phase;
			observation.occurrence = 0;
			observation.ordinal = ordinal;
			observation.value = value;
			return observation;
		}

		// The shape a battle actually produces: one live sound per object, its key fixed for the sound's
		// life and its reading moving every tick. Keys are spread over a realistic UID range and share the
		// handful of script-hook hashes a phase comes from.
		std::vector<NetSoundObservation> MakeObservationSet(uint8_t sender, size_t count, uint64_t startTick, float bias) {
			static const uint64_t phases[4] = {0x9E3779B97F4A7C15ULL, 0xC2B2AE3D27D4EB4FULL, 0x165667B19E3779F9ULL, 0x27D4EB2F165667C5ULL};
			std::vector<NetSoundObservation> observations;
			observations.reserve(count);
			for (size_t i = 0; i < count; ++i) {
				observations.push_back(MakeObservation(sender, 1048576 + static_cast<uint64_t>(i) * 3, startTick + i % 7, phases[i % 4], 1 + i % 5,
				                                       0.25F + static_cast<float>((i + static_cast<size_t>(bias * 64.0F)) % 64) / 256.0F));
			}
			return observations;
		}

		size_t FrameBytesWithoutObservations(const NetLockstepFrame& frame) {
			NetLockstepFrame stripped = frame;
			stripped.observations.clear();
			std::vector<uint8_t> bytes;
			return NetLockstepCodec::Encode({stripped}, bytes) ? bytes.size() : 0;
		}

		// The same frame as a version 13 packet: identical up to the observations, which spell out five
		// full-width key fields and a reading each. Everything before the block is byte for byte what the
		// current encoder writes, so this is a real recording's shape and not a guess at one.
		std::vector<uint8_t> MakeVersion13Frame(const NetLockstepFrame& frame) {
			NetLockstepFrame stripped = frame;
			stripped.observations.clear();
			std::vector<uint8_t> bytes;
			if (!NetLockstepCodec::Encode({stripped}, bytes)) {
				return {};
			}
			bytes.resize(bytes.size() - 3); // The empty observation block: a zero binding sequence and a zero count.
			const auto appendU16 = [&bytes](uint16_t value) { bytes.push_back(static_cast<uint8_t>(value)); bytes.push_back(static_cast<uint8_t>(value >> 8)); };
			const auto appendU32 = [&bytes](uint32_t value) { for (int i = 0; i < 4; ++i) { bytes.push_back(static_cast<uint8_t>(value >> (i * 8))); } };
			const auto appendU64 = [&bytes](uint64_t value) { for (int i = 0; i < 8; ++i) { bytes.push_back(static_cast<uint8_t>(value >> (i * 8))); } };
			appendU16(static_cast<uint16_t>(frame.observations.size()));
			for (const NetSoundObservation& observation : frame.observations) {
				appendU64(observation.objectUID);
				appendU64(observation.tick);
				appendU64(observation.phase);
				appendU64(observation.occurrence);
				appendU64(observation.ordinal);
				uint32_t valueBits = 0;
				std::memcpy(&valueBits, &observation.value, sizeof(valueBits));
				appendU32(valueBits);
			}
			bytes[4] = 13;
			bytes[5] = 0;
			const uint32_t payloadLength = static_cast<uint32_t>(bytes.size() - NetLockstepCodec::c_HeaderBytes);
			for (int i = 0; i < 4; ++i) {
				bytes[12 + i] = static_cast<uint8_t>(payloadLength >> (i * 8));
			}
			return bytes;
		}

		NetLockstepFrame MakeObservationFrame(uint64_t targetFrame, uint64_t roundId, std::vector<NetSoundObservation> observations) {
			NetLockstepFrame frame;
			frame.senderPeerId = 2;
			frame.targetFrame = targetFrame;
			frame.roundId = roundId;
			frame.frames = {MakeFrame(100, 1)};
			frame.observations = std::move(observations);
			return frame;
		}

		class RecoveryTapTransport : public LoopbackTransport {
		public:
			struct SentChunk {
				NetPeerId peer;
				NetTransportLane lane;
				NetLockstepRecoveryChunk chunk;
				bool accepted;
			};

			size_t sendAttempts = 0;
			std::vector<SentChunk> recovery;

			bool Send(NetPeerId peer, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error = nullptr, bool* congested = nullptr) override {
				++sendAttempts;
				const bool accepted = LoopbackTransport::Send(peer, lane, bytes, error, congested);
				const auto decoded = NetLockstepCodec::Decode(bytes);
				if (decoded.ok) if (const auto* chunk = std::get_if<NetLockstepRecoveryChunk>(&decoded.packet.payload)) recovery.push_back({peer, lane, *chunk, accepted});
				return accepted;
			}
		};

		bool CheckRecoveredInput(const RecoveryTapTransport& transport, const NetLockstepFrame& expected, uint64_t sessionId, bool complete, std::string* error) {
			std::vector<uint8_t> canonical, accepted;
			NetLockstepError codecError;
			if (!NetLockstepCodec::EncodeRecoveryInput(expected, canonical, &codecError) || transport.recovery.empty()) {
				*error = "the readoption fixture did not produce a complete-input recovery packet";
				return false;
			}
			bool refused = false;
			for (const auto& sent: transport.recovery) {
				const auto& chunk = sent.chunk;
				if (sent.peer != 1 || sent.lane != NetTransportLane::ControlReliable || chunk.sessionId != sessionId ||
				    chunk.senderPeerId != expected.senderPeerId || chunk.roundId != expected.roundId || chunk.targetFrame != expected.targetFrame ||
				    chunk.totalBytes != canonical.size() || chunk.offset != accepted.size() || chunk.bytes.empty() ||
				    chunk.bytes.size() > canonical.size() - accepted.size() ||
				    !std::equal(chunk.bytes.begin(), chunk.bytes.end(), canonical.begin() + static_cast<std::ptrdiff_t>(accepted.size()))) {
					*error = "recovery changed its sender, session, round, target, payload or ordered retry offset";
					return false;
				}
				if (sent.accepted) accepted.insert(accepted.end(), chunk.bytes.begin(), chunk.bytes.end());
				else refused = true;
			}
			if (!complete) {
				if (refused && accepted.size() < canonical.size()) return true;
				*error = "the readoption fixture did not retain a refused recovery send";
				return false;
			}
			NetLockstepFrame decoded;
			if (accepted != canonical || !NetLockstepCodec::DecodeRecoveryInput(accepted, decoded, &codecError) || !(decoded == expected)) {
				*error = "the reassembled recovered input differs from the original controller, commands or observations";
				return false;
			}
			return true;
		}

		bool RecoveryWireCheck(bool condition, std::string* error, const std::string& message) {
			if (!condition) *error = "recovery wire: " + message;
			return condition;
		}

		NetLockstepFrame RecoveryWireInput(uint8_t peer, uint64_t target, uint64_t round, size_t observations = 3) {
			NetLockstepFrame input{peer, target, {MakeFrame(peer * 100, uint64_t{1} << WEAPON_FIRE), MakeFrame(peer * 100 + 1, 3)},
			                       {{peer, NetGameSetTeamFunds{0, 4100}, target * 4 + 1}, {peer, NetGameSetTeamFunds{1, 4200}, target * 4 + 2}}, round,
			                       MakeObservationSet(peer, observations, target, 0.25F)};
			input.frames[0].aimAngle = -0.0F;
			if (!input.observations.empty()) input.observations[0].value = -0.0F;
			return input;
		}

		bool SameRecoveryInputs(std::vector<NetLockstepFrame> actual, std::vector<NetLockstepFrame> expected) {
			const auto order = [](const auto& a, const auto& b) { return std::tie(a.targetFrame, a.senderPeerId) < std::tie(b.targetFrame, b.senderPeerId); };
			std::sort(actual.begin(), actual.end(), order);
			std::sort(expected.begin(), expected.end(), order);
			if (actual.size() != expected.size()) return false;
			for (size_t index = 0; index < actual.size(); ++index) {
				std::vector<uint8_t> a, b;
				if (!NetLockstepCodec::EncodeRecoveryInput(actual[index], a) || !NetLockstepCodec::EncodeRecoveryInput(expected[index], b) || a != b) return false;
			}
			return true;
		}

		std::vector<NetLockstepRecoveryChunk> RecoveryWireChunks(const NetLockstepFrame& input, uint64_t session) {
			std::vector<uint8_t> bytes;
			if (!NetLockstepCodec::EncodeRecoveryInput(input, bytes)) return {};
			std::vector<NetLockstepRecoveryChunk> chunks;
			for (size_t offset = 0; offset < bytes.size(); offset += NetLockstepCodec::c_MaxRecoveryChunkBytes) {
				NetLockstepRecoveryChunk chunk;
				chunk.senderPeerId = input.senderPeerId; chunk.sessionId = session; chunk.roundId = input.roundId; chunk.targetFrame = input.targetFrame;
				chunk.totalBytes = static_cast<uint32_t>(bytes.size()); chunk.offset = static_cast<uint32_t>(offset);
				chunk.bytes.assign(bytes.begin() + offset, bytes.begin() + std::min(bytes.size(), offset + NetLockstepCodec::c_MaxRecoveryChunkBytes));
				chunks.push_back(std::move(chunk));
			}
			return chunks;
		}

		class RecoveryWireTransport : public RecoveryTapTransport {
		public:
			std::vector<NetTransportEvent> injected;
			std::vector<NetTransportEvent> PollEvents() override {
				auto events = LoopbackTransport::PollEvents();
				events.insert(events.begin(), injected.begin(), injected.end());
				injected.clear();
				return events;
			}
		};

		struct RecoveryWireRound {
			RecoveryWireTransport transport[4];
			NetLockstepCoordinator peer[4];
			NetLockstepConfig config[4];
			uint8_t count = 0;
			uint64_t now = 0;

			void Pump(unsigned steps = 4) {
				for (unsigned step = 0; step < steps; ++step) {
					now += 5;
					for (uint8_t index = 0; index < count; ++index) transport[index].AdvanceTimeMs(5);
					for (uint8_t index = 0; index < count; ++index) peer[index].Tick(now);
				}
			}

			bool Start(uint8_t peerCount, uint16_t port, std::string* error, bool prime = true) {
				count = peerCount;
				if (!transport[0].StartHost(port, error)) return false;
				for (uint8_t index = 1; index < count; ++index) if (!transport[index].Connect("loopback", port, error)) return false;
				for (uint8_t index = 0; index < count; ++index) {
					auto& c = config[index];
					c.sessionId = 0x5257430000000000ULL + port; c.roundId = index == 0 ? c.sessionId + 1 : 0;
					c.startFrame = 41; c.resumeFromSnapshot = true; c.timeoutMs = 60000; c.localPeerId = index + 1; c.peerCount = count;
					c.relayToOtherPeers = index == 0; c.frameLane = NetTransportLane::ControlReliable;
					c.scenario = "LockstepSelfTest"; c.ownershipPolicy = "unique-id-split";
					if (index == 0) for (uint8_t remote = 1; remote < count; ++remote) c.remoteTransportPeerIds.emplace(remote + 1, remote);
					else c.remoteTransportPeerIds.emplace(1, 1);
					if (!peer[index].Start(transport[index], c, error)) return false;
				}
				Pump(10);
				for (uint8_t index = 0; index < count; ++index) {
					if (!RecoveryWireCheck(peer[index].IsRunning(), error, "fixture did not start")) return false;
					if (prime && !peer[index].PrimeResyncInputs({}, error)) return false;
				}
				return true;
			}

			void Inject(uint8_t receiver, NetPeerId from, NetTransportLane lane, const std::vector<uint8_t>& bytes) {
				transport[receiver].injected.push_back({NetTransportEventType::PacketReceived, from, lane, bytes, {}});
			}
		};

		std::vector<uint64_t> RecoveryDictionaryState(const NetSoundObservationTables& tables) {
			std::vector<uint64_t> state{tables.roundId, tables.transportSender, tables.bySender.size()};
			for (const auto& [peer, dictionary]: tables.bySender) {
				state.push_back(peer); state.push_back(dictionary.BindingCount());
				for (uint16_t slot = 0; slot < NetSoundObservationDictionary::c_MaxSlots; ++slot) {
					NetSoundObservationKey key;
					if (dictionary.Resolve(slot, key)) state.insert(state.end(), {slot, key.objectUID, key.tick, key.phase, key.occurrence, key.ordinal});
				}
			}
			return state;
		}

		bool TestRecoveryWireRefusals(std::string* error) {
			enum Mutation { ForgedSender, UnboundHost, UnboundClient, OldSession, OldRound, WrongLane, Reserved8, Reserved16, ShortChunk,
			                ZeroTotal, ExcessTotal, MisalignedOffset, MissingFirst, ChangedTotal, ConflictingRetry, IdenticalRetry,
			                OuterSender, OuterTarget, InnerRound, UnboundMalformed, StaleDuringAssembly, SessionDuringAssembly, FutureTarget };
			struct Case { Mutation mutation; const char* name; bool fatal; bool codecValid = true; };
			const std::vector<Case> cases{
				{ForgedSender, "bound transport forges another sender", false}, {UnboundHost, "unbound transport claims a client", false},
				{UnboundClient, "unbound transport claims the host", false}, {OldSession, "wrong session", false}, {OldRound, "wrong round", false},
				{WrongLane, "unreliable recovery", true}, {Reserved8, "reserved byte", true, false}, {Reserved16, "reserved word", true, false},
				{ShortChunk, "short nonfinal chunk", true, false}, {ZeroTotal, "zero total", true, false}, {ExcessTotal, "oversized total", true, false},
				{MisalignedOffset, "misaligned overlap", true, false}, {MissingFirst, "missing first chunk", true}, {ChangedTotal, "changed assembly total", true},
				{ConflictingRetry, "conflicting repeated chunk", true}, {IdenticalRetry, "identical repeated chunk", false},
				{OuterSender, "outer and inner senders differ", true}, {OuterTarget, "outer and inner targets differ", true},
				{InnerRound, "outer and inner rounds differ", true}, {UnboundMalformed, "malformed admission", false, false},
				{StaleDuringAssembly, "stale chunk during assembly", false}, {SessionDuringAssembly, "wrong session during assembly", false},
				{FutureTarget, "target outside pending window", false}
			};
			for (size_t index = 0; index < cases.size(); ++index) {
				const auto& test = cases[index];
				RecoveryWireRound round;
				if (!round.Start(3, static_cast<uint16_t>(44900 + index), error)) return false;
				const uint8_t receiver = test.mutation == UnboundClient ? 1 : 0;
				auto& coordinator = round.peer[receiver];
				const uint8_t seedSender = receiver == 0 ? 2 : 1;
				auto seed = RecoveryWireInput(seedSender, 44, coordinator.GetRoundId());
				NetSoundObservationDictionary encoder;
				std::vector<uint8_t> seedWire;
				if (!NetLockstepCodec::Encode({seed}, seedWire, nullptr, &encoder)) return RecoveryWireCheck(false, error, "dictionary seed did not encode");
				round.Inject(receiver, 1, NetTransportLane::ControlReliable, seedWire);
				coordinator.Tick(++round.now);
				const auto before = coordinator.CapturePendingInputs(40);
				if (!RecoveryWireCheck(SameRecoveryInputs(before, {seed}), error, "live dictionary seed was not received")) return false;
				NetSoundObservationTables tables;
				tables.roundId = coordinator.GetRoundId();
				if (!NetLockstepCodec::Decode(seedWire, ControllerFrame::c_Version, &tables).ok) return RecoveryWireCheck(false, error, "codec dictionary seed did not decode");
				tables.Exactly(3).Bind(0, {7654, 3, 5, 7, 9});
				const auto dictionaryBefore = RecoveryDictionaryState(tables);
				const auto statsBefore = coordinator.GetStats();
				const auto historyBefore = coordinator.CaptureLocalInputHistory();
				const auto acksBefore = coordinator.GetAuthoritativeCommandAcks();
				auto input = RecoveryWireInput(seedSender, 41, coordinator.GetRoundId(), 4096);
				const auto chunks = RecoveryWireChunks(input, round.config[0].sessionId);
				if (!RecoveryWireCheck(chunks.size() >= 3, error, "negative fixture did not span three chunks")) return false;
				std::vector<NetLockstepRecoveryChunk> selected{chunks.front()};
				NetPeerId from = 1;
				NetTransportLane lane = NetTransportLane::ControlReliable;
				bool finish = false;
				switch (test.mutation) {
					case ForgedSender: selected[0].senderPeerId = 3; break;
					case UnboundHost: case UnboundClient: case UnboundMalformed: from = 99; break;
					case OldSession: ++selected[0].sessionId; break;
					case OldRound: ++selected[0].roundId; break;
					case WrongLane: lane = NetTransportLane::InputUnreliable; break;
					case MissingFirst: selected = {chunks[1]}; break;
					case ChangedTotal: selected.push_back(chunks[1]); --selected.back().totalBytes; break;
					case ConflictingRetry: selected.push_back(chunks[0]); selected.back().bytes.back() ^= 1; break;
					case IdenticalRetry: selected.push_back(chunks[0]); finish = true; break;
					case OuterSender: selected = chunks; from = 2; for (auto& chunk: selected) chunk.senderPeerId = 3; break;
					case OuterTarget: selected = chunks; for (auto& chunk: selected) ++chunk.targetFrame; break;
					case InnerRound: ++input.roundId; selected = RecoveryWireChunks(input, round.config[0].sessionId); for (auto& chunk: selected) --chunk.roundId; break;
					case StaleDuringAssembly: selected.push_back(chunks[1]); ++selected.back().roundId; finish = true; break;
					case SessionDuringAssembly: selected.push_back(chunks[1]); ++selected.back().sessionId; finish = true; break;
					case FutureTarget: selected[0].targetFrame = coordinator.GetStats().nextFrame + NetLockstepCodec::c_MaxFutureFrameSkew + 1; break;
					default: break;
				}
				for (const auto& chunk: selected) {
					std::vector<uint8_t> wire;
					if (!EncodePacket({chunk}, wire, error)) return false;
					const auto put32 = [&](size_t offset, uint32_t value) { for (size_t byte = 0; byte < 4; ++byte) wire[offset + byte] = static_cast<uint8_t>(value >> (byte * 8)); };
					const size_t header = NetLockstepCodec::c_HeaderBytes;
					switch (test.mutation) {
						case Reserved8: case UnboundMalformed: wire[header + 1] = 1; break;
						case Reserved16: wire[header + 2] = 1; break;
						case ShortChunk: wire.pop_back(); put32(12, static_cast<uint32_t>(wire.size() - header)); break;
						case ZeroTotal: put32(header + 28, 0); break;
						case ExcessTotal: put32(header + 28, static_cast<uint32_t>(NetLockstepCodec::c_MaxRecoveryInputBytes + 1)); break;
						case MisalignedOffset: put32(header + 32, 1); break;
						default: break;
					}
					const auto decoded = NetLockstepCodec::Decode(wire, ControllerFrame::c_Version, &tables);
					if (!RecoveryWireCheck(decoded.ok == test.codecValid && RecoveryDictionaryState(tables) == dictionaryBefore, error,
					                       std::string(test.name) + " changed dictionaries or missed its codec control")) return false;
					round.Inject(receiver, from, lane, wire);
				}
				coordinator.Tick(++round.now);
				const auto& after = coordinator.GetStats();
				NetLockstepReadyFrame ready;
				if (!RecoveryWireCheck(SameRecoveryInputs(coordinator.CapturePendingInputs(40), before) &&
				                       SameRecoveryInputs(coordinator.CaptureLocalInputHistory(), historyBefore) && !coordinator.PopReadyFrame(ready) &&
				                       after.nextFrame == statsBefore.nextFrame && after.framesAccepted == statsBefore.framesAccepted &&
				                       after.framePacketsReceived == statsBefore.framePacketsReceived && after.remoteControllerFramesReceived == statsBefore.remoteControllerFramesReceived &&
				                       coordinator.GetAuthoritativeCommandAcks() == acksBefore && after.unresolvedObservationPackets == statsBefore.unresolvedObservationPackets &&
				                       (test.fatal ? coordinator.IsFailed() && after.timeoutReason.find("ProtocolError") == 0 : coordinator.IsRunning()),
				                       error, std::string(test.name) + " changed live input or terminal policy")) return false;
				if (test.mutation == ForgedSender && !RecoveryWireCheck(after.peers.at(3).lastHeardMs == statsBefore.peers.at(3).lastHeardMs, error, "forgery refreshed its victim")) return false;
				const bool stale = test.mutation == OldSession || test.mutation == OldRound || test.mutation == StaleDuringAssembly || test.mutation == SessionDuringAssembly;
				if (!RecoveryWireCheck(after.staleRoundPackets == statsBefore.staleRoundPackets + (stale ? 1 : 0), error, "stale recovery guard was not exercised")) return false;
				if (!RecoveryWireCheck(after.futureFrameDrops == statsBefore.futureFrameDrops + (test.mutation == FutureTarget ? 1 : 0), error, "future recovery guard was not exercised")) return false;
				auto canary = seed;
				canary.targetFrame = 45;
				for (auto& observation: canary.observations) observation.value = 0.75F;
				std::vector<uint8_t> referenceWire;
				if (!NetLockstepCodec::Encode({canary}, referenceWire, nullptr, &encoder)) return false;
				const auto reference = NetLockstepCodec::Decode(referenceWire, ControllerFrame::c_Version, &tables);
				const auto* referenceFrame = reference.ok ? std::get_if<NetLockstepFrame>(&reference.packet.payload) : nullptr;
				if (!RecoveryWireCheck(referenceWire.size() < seedWire.size() && referenceFrame && SameRecoveryInputs({*referenceFrame}, {canary}), error, "slot references changed after refused recovery")) return false;
				if (!test.fatal) {
					round.Inject(receiver, 1, NetTransportLane::ControlReliable, referenceWire);
					coordinator.Tick(++round.now);
					if (!RecoveryWireCheck(coordinator.IsRunning() && SameRecoveryInputs(coordinator.CapturePendingInputs(40), {seed, canary}), error, "live slot references changed after ignored recovery")) return false;
				}
				if (finish) {
					for (size_t next = 1; next < chunks.size(); ++next) {
						std::vector<uint8_t> wire;
						if (!EncodePacket({chunks[next]}, wire, error)) return false;
						round.Inject(receiver, 1, NetTransportLane::ControlReliable, wire);
					}
					coordinator.Tick(++round.now);
					if (!RecoveryWireCheck(coordinator.IsRunning() && SameRecoveryInputs(coordinator.CapturePendingInputs(40), {seed, canary, input}) &&
					                       coordinator.GetStats().framePacketsReceived == statsBefore.framePacketsReceived + 2,
					                       error, "ignored chunk poisoned or duplicated the completed input")) return false;
				}
			}
			std::cout << "[net-lockstep-selftest] PASS recovery_wire_refusals cases=" << cases.size() << std::endl;
			return true;
		}

		bool CheckRecoveryRoute(const RecoveryTapTransport& transport, NetPeerId destination, const std::vector<NetLockstepFrame>& expected,
		                        uint64_t session, std::string* error) {
			using Key = std::pair<uint8_t, uint64_t>;
			std::map<Key, std::vector<uint8_t>> canonical;
			std::map<Key, size_t> accepted;
			std::map<uint8_t, uint64_t> lastTarget;
			for (const auto& input: expected) if (!NetLockstepCodec::EncodeRecoveryInput(input, canonical[{input.senderPeerId, input.targetFrame}])) return false;
			for (const auto& sent: transport.recovery) {
				if (sent.peer != destination) continue;
				const auto& chunk = sent.chunk;
				const Key key{chunk.senderPeerId, chunk.targetFrame};
				const auto input = std::find_if(expected.begin(), expected.end(), [&](const auto& value) { return value.senderPeerId == key.first && value.targetFrame == key.second; });
				if (!RecoveryWireCheck(input != expected.end(), error, "relay sent an input back to its author or to the wrong destination")) return false;
				const auto& bytes = canonical.at(key);
				const auto previous = lastTarget.find(chunk.senderPeerId);
				if (previous != lastTarget.end() && previous->second != chunk.targetFrame) {
					const Key prior{chunk.senderPeerId, previous->second};
					if (!RecoveryWireCheck(previous->second < chunk.targetFrame && accepted[prior] == canonical.at(prior).size(), error, "relay reordered targets before finishing an input")) return false;
				}
				lastTarget[chunk.senderPeerId] = chunk.targetFrame;
				const size_t offset = accepted[key];
				if (!RecoveryWireCheck(sent.lane == NetTransportLane::ControlReliable && chunk.sessionId == session && chunk.roundId == input->roundId &&
				                       chunk.totalBytes == bytes.size() && chunk.offset == offset && !chunk.bytes.empty() && chunk.bytes.size() <= bytes.size() - offset &&
				                       std::equal(chunk.bytes.begin(), chunk.bytes.end(), bytes.begin() + offset), error, "relay changed scope, bytes or its refused retry offset")) return false;
				if (sent.accepted) accepted[key] += chunk.bytes.size();
			}
			for (const auto& [key, bytes]: canonical) if (!RecoveryWireCheck(accepted[key] == bytes.size(), error, "relay did not finish every recipient's original input")) return false;
			return true;
		}

		bool TestRecoveryWireRelayRetry(std::string* error) {
			for (uint8_t count: {3, 4}) {
				RecoveryWireRound round;
				if (!round.Start(count, static_cast<uint16_t>(44960 + count), error)) return false;
				std::vector<NetLockstepFrame> expected;
				for (uint8_t sender = 1; sender <= count; ++sender) for (uint64_t target: {41ULL, 42ULL}) expected.push_back(RecoveryWireInput(sender, target, round.peer[0].GetRoundId(), 4096));
				const auto queue = [&](uint8_t sender) {
					for (const auto& input: expected) if (input.senderPeerId == sender && !round.peer[sender - 1].QueueRecoveredInput(input, error)) return false;
					return true;
				};
				LoopbackTransportConfig refusal;
				refusal.refuseSendsToPeer = 2; refusal.acceptedSendsBeforeRefusing = 1;
				round.transport[0].SetFaultConfig(refusal);
				if (!queue(2)) return false;
				round.Pump(4);
				const auto first = std::find_if(expected.begin(), expected.end(), [](const auto& input) { return input.senderPeerId == 2 && input.targetFrame == 41; });
				const auto contains = [&](uint8_t receiver, const NetLockstepFrame& input) {
					const auto pending = round.peer[receiver - 1].CapturePendingInputs(40);
					return std::any_of(pending.begin(), pending.end(), [&](const auto& value) { return SameRecoveryInputs({value}, {input}); });
				};
				const auto& sends = round.transport[0].recovery;
				const auto refused = std::find_if(sends.begin(), sends.end(), [](const auto& send) { return send.peer == 2 && !send.accepted && send.chunk.offset != 0; });
				if (!RecoveryWireCheck(refused != sends.end() && !contains(3, *first) && contains(1, *first) && (count == 3 || contains(4, *first)),
				                       error, "relay refusal did not leave one recipient partial while a healthy route received the full input")) return false;
				for (uint8_t sender = 1; sender <= count; ++sender) if (sender != 2 && !queue(sender)) return false;
				round.Pump(4);
				NetLockstepReadyFrame ready;
				for (uint8_t index = 0; index < count; ++index) if (!RecoveryWireCheck(round.peer[index].IsRunning() && round.peer[index].GetStats().nextFrame == 41 &&
				                                                                 !round.peer[index].PopReadyFrame(ready), error, "partial relay committed an incomplete input")) return false;
				round.transport[0].SetFaultConfig(LoopbackTransportConfig{});
				round.Pump(40);
				for (uint8_t index = 0; index < count; ++index) {
					if (!RecoveryWireCheck(std::all_of(round.transport[index].recovery.begin(), round.transport[index].recovery.end(), [&](const auto& send) {
						return index == 0 ? send.peer >= 1 && send.peer < count : send.peer == 1;
					}), error, "recovery used a destination outside the round")) return false;
				}
				for (uint8_t receiver = 1; receiver <= count; ++receiver) {
					auto& peer = round.peer[receiver - 1];
					if (!RecoveryWireCheck(peer.IsRunning() && peer.GetStats().nextFrame == 43 && SameRecoveryInputs(peer.CapturePendingInputs(40), expected) &&
					                       peer.GetStats().framePacketsReceived == 2U * (count - 1) && peer.GetStats().remoteControllerFramesAccepted == 4U * (count - 1) &&
					                       peer.GetStats().observationsCarried == 0 && peer.GetStats().observationsDropped == 0 && peer.GetStats().relayBacklogBytes == 0,
					                       error, "relay changed a recipient's sender, target, controller, ordered commands or 4096 observations")) return false;
					for (uint64_t target: {41ULL, 42ULL}) if (!RecoveryWireCheck(peer.PopReadyFrame(ready) && ready.frame == target && ready.hasLocalInput &&
					                                                                        ready.remoteFrameCounts.size() == count - 1, error, "relay changed ready-frame boundaries")) return false;
					if (!RecoveryWireCheck(!peer.PopReadyFrame(ready) && peer.CapturePendingInputs(40).empty(), error, "relay retained or applied an input twice")) return false;
					std::vector<NetLockstepFrame> route;
					if (receiver == 1) continue;
					for (const auto& input: expected) if (input.senderPeerId != receiver) route.push_back(input);
					if (!CheckRecoveryRoute(round.transport[0], receiver - 1, route, round.config[0].sessionId, error)) return false;
					route.clear();
					for (const auto& input: expected) if (input.senderPeerId == receiver) route.push_back(input);
					if (!CheckRecoveryRoute(round.transport[receiver - 1], 1, route, round.config[0].sessionId, error)) return false;
				}
			}
			std::cout << "[net-lockstep-selftest] PASS recovery_wire_relay_retry peers=3,4 observations=4096 targets=2" << std::endl;
			return true;
		}

		bool TestRecoveryInputMembership(std::string* error) {
			{
				RecoveryWireRound round;
				if (!round.Start(2, 44950, error, false)) return false;
				auto& host = round.peer[0];
				const auto before = host.CapturePendingInputs(40);
				const auto sends = round.transport[0].sendAttempts;
				const auto unknown = RecoveryWireInput(3, 41, host.GetRoundId());
				std::vector<uint8_t> valid;
				std::string rejected;
				if (!RecoveryWireCheck(NetLockstepCodec::EncodeRecoveryInput(unknown, valid) && !host.InstallResyncInputs({unknown}, &rejected) && !rejected.empty() &&
				                       SameRecoveryInputs(host.CapturePendingInputs(40), before) && host.CaptureLocalInputHistory().empty() && host.NeedsResyncPriming() &&
				                       host.IsRunning() && host.GetStats().nextFrame == 41 && host.GetStats().framesAccepted == 0 && round.transport[0].sendAttempts == sends,
				                       error, "never-member input entered the round or changed the rejected installation")) return false;
				const std::vector<NetLockstepFrame> inputs{RecoveryWireInput(1, 41, host.GetRoundId()), RecoveryWireInput(2, 41, host.GetRoundId())};
				if (!host.InstallResyncInputs(inputs, error) || !host.PrimeResyncInputs({}, error)) return false;
				host.Tick(++round.now);
				if (!RecoveryWireCheck(host.IsRunning() && host.GetStats().nextFrame == 42 && SameRecoveryInputs(host.CapturePendingInputs(40), inputs),
				                       error, "refused never-member input poisoned a valid installation")) return false;
			}
			{
				RecoveryWireRound round;
				if (!round.Start(3, 44951, error, false)) return false;
				auto& host = round.peer[0];
				const auto accepted = RecoveryWireInput(3, 42, host.GetRoundId());
				if (!round.peer[2].QueueRecoveredInput(accepted, error)) return false;
				round.Pump(4);
				if (!RecoveryWireCheck(SameRecoveryInputs(host.CapturePendingInputs(40), {accepted}), error, "departed-input fixture never accepted its source input")) return false;
				round.transport[2].Disconnect(1, "membership control");
				round.Pump(4);
				if (!RecoveryWireCheck(host.IsRunning() && host.GetConfig().peerCount == 3 && host.IsPeerGoneAtFrame(3, 42) &&
				                       !host.UsesTransportPeer(2) && SameRecoveryInputs(host.CapturePendingInputs(40), {accepted}),
				                       error, "departed configured slot lost its accepted future input")) return false;
				host.ResolveHeldSeat(3, NetLockstepHoldResolution::Expired, round.now);
				std::vector<NetLockstepFrame> inputs{accepted};
				for (uint8_t sender: {1, 2}) for (uint64_t target: {41ULL, 42ULL}) inputs.push_back(RecoveryWireInput(sender, target, host.GetRoundId()));
				if (!host.InstallResyncInputs(inputs, error) || !host.PrimeResyncInputs({}, error)) return false;
				host.Tick(++round.now);
				if (!RecoveryWireCheck(host.IsRunning() && host.GetStats().nextFrame == 43 && SameRecoveryInputs(host.CapturePendingInputs(40), inputs),
				                       error, "restoration dropped a configured departed sender's accepted controller, commands or observations")) return false;
			}
			std::cout << "[net-lockstep-selftest] PASS recovery_input_membership never_member=refused accepted_departed=preserved" << std::endl;
			return true;
		}

		bool TestObservationSlotCodec(std::string* error) {
			const uint64_t roundId = 0x5EED0000C0FFEE14ULL;
			NetSoundObservationDictionary encoder;
			NetSoundObservationTables decoderTables;

			const auto roundTripThrough = [&](const NetLockstepFrame& frame, size_t& outBytes, size_t& outEncoded) {
				std::vector<uint8_t> bytes;
				NetLockstepError encodeError;
				outEncoded = frame.observations.size();
				if (!NetLockstepCodec::Encode({frame}, bytes, &encodeError, &encoder, &outEncoded)) {
					*error = "compact observation encode failed: " + encodeError.message;
					return false;
				}
				outBytes = bytes.size();
				const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(bytes, ControllerFrame::c_Version, &decoderTables);
				if (!decoded.ok) {
					*error = "compact observation decode failed: " + decoded.error.message;
					return false;
				}
				const NetLockstepFrame* out = std::get_if<NetLockstepFrame>(&decoded.packet.payload);
				if (!out || out->observations.size() != outEncoded) {
					*error = "compact observation decode returned the wrong count";
					return false;
				}
				for (size_t i = 0; i < outEncoded; ++i) {
					if (!(out->observations[i] == frame.observations[i])) {
						*error = "compact observation " + std::to_string(i) + " did not survive the wire";
						return false;
					}
				}
				return true;
			};

			// First use spells the key out; the same keys next frame are slot references only.
			const std::vector<NetSoundObservation> firstSet = MakeObservationSet(2, 64, 400, 0.0F);
			const NetLockstepFrame first = MakeObservationFrame(10, roundId, firstSet);
			const size_t emptyBytes = FrameBytesWithoutObservations(first);
			size_t firstBytes = 0;
			size_t firstEncoded = 0;
			if (!roundTripThrough(first, firstBytes, firstEncoded) || firstEncoded != 64) {
				if (error->empty()) { *error = "first observation frame did not encode every observation"; }
				return false;
			}
			std::vector<NetSoundObservation> repeatSet = firstSet;
			for (NetSoundObservation& observation : repeatSet) {
				observation.value += 0.001953125F; // A changed reading of the same sound.
			}
			size_t repeatBytes = 0;
			size_t repeatEncoded = 0;
			if (!roundTripThrough(MakeObservationFrame(11, roundId, repeatSet), repeatBytes, repeatEncoded) || repeatEncoded != 64) {
				if (error->empty()) { *error = "repeat observation frame did not encode every observation"; }
				return false;
			}
			// Five bytes each: a one-byte slot reference and the reading, after the two-byte count.
			if (repeatBytes - emptyBytes != 5 * 64) {
				*error = "a repeated observation did not cost five bytes: block=" + std::to_string(repeatBytes - emptyBytes);
				return false;
			}
			if (firstBytes <= repeatBytes || firstBytes - emptyBytes > 24 * 64) {
				*error = "a first-use observation block was not in its expected range: " + std::to_string(firstBytes - emptyBytes);
				return false;
			}

			// Filling the table evicts the key nobody has mentioned since; it returns spelled out in full.
			const NetSoundObservationKey evicted = KeyOfObservation(firstSet.front());
			uint64_t nextFrame = 12;
			for (size_t block = 0; block * 256 < NetSoundObservationDictionary::c_MaxSlots; ++block) {
				std::vector<NetSoundObservation> fresh;
				for (size_t i = 0; i < 256; ++i) {
					fresh.push_back(MakeObservation(2, 9000000 + block * 256 + i, 500 + i, 0x1234ULL + i, 1, 0.5F));
				}
				size_t bytes = 0;
				size_t encoded = 0;
				if (!roundTripThrough(MakeObservationFrame(nextFrame++, roundId, fresh), bytes, encoded) || encoded != fresh.size()) {
					if (error->empty()) { *error = "a slot-exhaustion frame did not encode every observation"; }
					return false;
				}
			}
			uint16_t stillBound = 0;
			if (encoder.Lookup(evicted, stillBound)) {
				*error = "the least recently used key was not evicted once every slot was bound";
				return false;
			}
			size_t returnBytes = 0;
			size_t returnEncoded = 0;
			if (!roundTripThrough(MakeObservationFrame(nextFrame++, roundId, {firstSet.front()}), returnBytes, returnEncoded) || returnEncoded != 1) {
				if (error->empty()) { *error = "a returning key did not encode"; }
				return false;
			}
			if (returnBytes - emptyBytes <= 5) {
				*error = "a returning key was sent as a bare slot reference";
				return false;
			}

			// More first-use observations than the byte budget holds: the encoder stops, says how many it
			// took, and the rest encode next frame against a table that never saw them.
			std::vector<NetSoundObservation> flood;
			for (size_t i = 0; i < NetLockstepCodec::c_MaxObservationsPerPacket; ++i) {
				flood.push_back(MakeObservation(2, 20000000 + i * 7, 900 + i, 0xABCDEF0123456789ULL + i, 1 + i % 3, 0.75F));
			}
			size_t floodBytes = 0;
			size_t floodEncoded = 0;
			if (!roundTripThrough(MakeObservationFrame(nextFrame++, roundId, flood), floodBytes, floodEncoded)) {
				return false;
			}
			if (floodEncoded == 0 || floodEncoded >= flood.size()) {
				*error = "the observation byte budget did not stop a flood of new keys: encoded " + std::to_string(floodEncoded);
				return false;
			}
			// The slack is the block's own binding sequence, which is wider here than in an empty block.
			if (floodBytes - emptyBytes > NetLockstepCodec::c_MaxObservationBytesPerPacket + 16 ||
			    floodBytes > NetLockstepCodec::c_HeaderBytes + NetLockstepCodec::c_MaxPayloadBytes) {
				*error = "an observation block passed its byte budget";
				return false;
			}
			const std::vector<NetSoundObservation> carried(flood.begin() + static_cast<std::ptrdiff_t>(floodEncoded), flood.end());
			size_t carriedBytes = 0;
			size_t carriedEncoded = 0;
			if (!roundTripThrough(MakeObservationFrame(nextFrame++, roundId, carried), carriedBytes, carriedEncoded)) {
				return false;
			}
			if (carriedEncoded == 0) {
				*error = "the carried remainder encoded nothing";
				return false;
			}

			// A version 13 recording still decodes, with and without a table to read slots against.
			const NetLockstepFrame legacy = MakeObservationFrame(40, roundId, MakeObservationSet(2, 5, 77, 0.5F));
			const std::vector<uint8_t> legacyBytes = MakeVersion13Frame(legacy);
			// One byte less than an empty version 15 block, because version 13 has no binding sequence.
			if (legacyBytes.size() != FrameBytesWithoutObservations(legacy) - 1 + 44 * 5) {
				*error = "the version 13 frame is not forty-four bytes per observation";
				return false;
			}
			for (NetSoundObservationTables* tables: {static_cast<NetSoundObservationTables*>(nullptr), &decoderTables}) {
				const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(legacyBytes, ControllerFrame::c_Version, tables);
				if (!decoded.ok) {
					*error = "a version 13 frame did not decode: " + decoded.error.message;
					return false;
				}
				const NetLockstepFrame* out = std::get_if<NetLockstepFrame>(&decoded.packet.payload);
				if (!out || out->observations != legacy.observations) {
					*error = "a version 13 frame decoded to different observations";
					return false;
				}
			}

			// A slot reference nobody spelled out is refused, whether the table is missing or just lacks it.
			NetSoundObservationDictionary lone;
			std::vector<uint8_t> bindBytes;
			size_t loneEncoded = 0;
			if (!NetLockstepCodec::Encode({MakeObservationFrame(50, 0, MakeObservationSet(2, 2, 3, 0.0F))}, bindBytes, nullptr, &lone, &loneEncoded)) {
				*error = "could not encode the binding frame";
				return false;
			}
			NetSoundObservationTables mirror;
			if (!NetLockstepCodec::Decode(bindBytes, ControllerFrame::c_Version, &mirror).ok) {
				*error = "could not decode the binding frame";
				return false;
			}
			std::vector<uint8_t> refBytes;
			size_t refEncoded = 0;
			if (!NetLockstepCodec::Encode({MakeObservationFrame(51, 0, MakeObservationSet(2, 2, 3, 0.25F))}, refBytes, nullptr, &lone, &refEncoded)) {
				*error = "could not encode the slot-reference frame";
				return false;
			}
			if (NetLockstepCodec::Decode(refBytes, ControllerFrame::c_Version, nullptr).error.code != NetLockstepErrorCode::UnboundObservationSlot) {
				*error = "a slot reference without any table was not refused";
				return false;
			}
			// A table that has never been fed sees the gap before it ever reaches the slot.
			NetSoundObservationTables emptyTables;
			if (NetLockstepCodec::Decode(refBytes, ControllerFrame::c_Version, &emptyTables).error.code != NetLockstepErrorCode::ObservationBindingGap) {
				*error = "a slot reference against an empty table was not refused";
				return false;
			}
			if (!NetLockstepCodec::Decode(refBytes, ControllerFrame::c_Version, &mirror).ok) {
				*error = "a slot reference did not decode against the table that bound it";
				return false;
			}

			// The one packet that says a slot has been reused is the only place that is ever said, so a
			// receiver that misses it must refuse rather than read the slot as the key it held before.
			// This is the reviewer's lost_rebinding probe: it decodes with no error on version 14.
			uint64_t silentlyWrongKey = 0;
			{
				NetSoundObservationDictionary sender;
				NetSoundObservationTables receiver;
				uint64_t nextFrame = 100;
				// Bind every slot, so the next key handed out reuses the one nobody has mentioned since.
				for (size_t block = 0; block * 512 < NetSoundObservationDictionary::c_MaxSlots; ++block) {
					std::vector<NetSoundObservation> keys;
					for (size_t i = 0; i < 512; ++i) {
						keys.push_back(MakeObservation(2, 700000 + block * 512 + i, 11, 0x5151ULL + i, 1, 0.125F));
					}
					std::vector<uint8_t> bytes;
					size_t encoded = 0;
					if (!NetLockstepCodec::Encode({MakeObservationFrame(nextFrame++, roundId, keys)}, bytes, nullptr, &sender, &encoded) || encoded != keys.size()) {
						*error = "the rebinding probe could not fill the table";
						return false;
					}
					if (!NetLockstepCodec::Decode(bytes, ControllerFrame::c_Version, &receiver).ok) {
						*error = "the rebinding probe's filling frames did not decode";
						return false;
					}
				}
				const NetSoundObservation reused = MakeObservation(2, 990001, 12, 0x6262ULL, 1, 0.25F);
				std::vector<uint8_t> rebinding;
				size_t rebindingEncoded = 0;
				if (!NetLockstepCodec::Encode({MakeObservationFrame(nextFrame++, roundId, {reused})}, rebinding, nullptr, &sender, &rebindingEncoded)) {
					*error = "the rebinding frame did not encode";
					return false;
				}
				// The receiver never gets that frame. The next one refers to the reused slot.
				NetSoundObservation later = reused;
				later.value = 0.5F;
				std::vector<uint8_t> after;
				size_t afterEncoded = 0;
				if (!NetLockstepCodec::Encode({MakeObservationFrame(nextFrame++, roundId, {later})}, after, nullptr, &sender, &afterEncoded)) {
					*error = "the frame after the rebinding did not encode";
					return false;
				}
				const NetLockstepDecodeResult lost = NetLockstepCodec::Decode(after, ControllerFrame::c_Version, &receiver);
				if (lost.ok || lost.error.code != NetLockstepErrorCode::ObservationBindingGap) {
					*error = "a lost rebinding was not refused: ok=" + std::to_string(lost.ok ? 1 : 0) + " " + NetLockstepCodec::ErrorCodeName(lost.error.code);
					return false;
				}
				// The control: the same packet as version 14 said nothing about the bindings behind it, so the
				// same receiver reads the reused slot as the key it held before and commits the sent reading
				// against the wrong sound, with no error at all. That is what the sequence above refuses.
				const size_t prefix = FrameBytesWithoutObservations(MakeObservationFrame(0, roundId, {})) - 3;
				std::vector<uint8_t> asVersion14 = after;
				size_t sequenceBytes = 0;
				while (prefix + sequenceBytes < asVersion14.size() && (asVersion14[prefix + sequenceBytes] & 0x80U) != 0) {
					++sequenceBytes;
				}
				++sequenceBytes;
				asVersion14.erase(asVersion14.begin() + static_cast<std::ptrdiff_t>(prefix),
				                  asVersion14.begin() + static_cast<std::ptrdiff_t>(prefix + sequenceBytes));
				asVersion14[4] = 14;
				for (int i = 0; i < 4; ++i) {
					asVersion14[12 + i] = static_cast<uint8_t>((asVersion14.size() - NetLockstepCodec::c_HeaderBytes) >> (i * 8));
				}
				const NetLockstepDecodeResult silent = NetLockstepCodec::Decode(asVersion14, ControllerFrame::c_Version, &receiver);
				const NetLockstepFrame* silentFrame = silent.ok ? std::get_if<NetLockstepFrame>(&silent.packet.payload) : nullptr;
				if (!silentFrame || silentFrame->observations.size() != 1) {
					*error = "the version 14 control did not decode, so it proves nothing";
					return false;
				}
				if (silentFrame->observations.front().objectUID == later.objectUID) {
					*error = "the version 14 control did not reproduce the wrong key it is there to show";
					return false;
				}
				silentlyWrongKey = silentFrame->observations.front().objectUID;
				// Delivered in order, the same two frames read exactly what the sender meant.
				NetSoundObservationDictionary replaySender;
				NetSoundObservationTables replayReceiver;
				uint64_t replayFrame = 200;
				bool replayOk = true;
				for (size_t block = 0; block * 512 < NetSoundObservationDictionary::c_MaxSlots && replayOk; ++block) {
					std::vector<NetSoundObservation> keys;
					for (size_t i = 0; i < 512; ++i) {
						keys.push_back(MakeObservation(2, 700000 + block * 512 + i, 11, 0x5151ULL + i, 1, 0.125F));
					}
					std::vector<uint8_t> bytes;
					size_t encoded = 0;
					replayOk = NetLockstepCodec::Encode({MakeObservationFrame(replayFrame++, roundId, keys)}, bytes, nullptr, &replaySender, &encoded) &&
					           NetLockstepCodec::Decode(bytes, ControllerFrame::c_Version, &replayReceiver).ok;
				}
				std::vector<uint8_t> replayRebinding, replayAfter;
				size_t ignored = 0;
				replayOk = replayOk &&
				           NetLockstepCodec::Encode({MakeObservationFrame(replayFrame++, roundId, {reused})}, replayRebinding, nullptr, &replaySender, &ignored) &&
				           NetLockstepCodec::Decode(replayRebinding, ControllerFrame::c_Version, &replayReceiver).ok &&
				           NetLockstepCodec::Encode({MakeObservationFrame(replayFrame++, roundId, {later})}, replayAfter, nullptr, &replaySender, &ignored);
				if (!replayOk) {
					*error = "the in-order replay of the rebinding stream failed";
					return false;
				}
				const NetLockstepDecodeResult delivered = NetLockstepCodec::Decode(replayAfter, ControllerFrame::c_Version, &replayReceiver);
				const NetLockstepFrame* deliveredFrame = delivered.ok ? std::get_if<NetLockstepFrame>(&delivered.packet.payload) : nullptr;
				if (!deliveredFrame || deliveredFrame->observations.size() != 1 || !(deliveredFrame->observations.front() == later)) {
					*error = "the reused slot did not read as the key the sender rebound it to";
					return false;
				}
			}

			// A sender that starts its round over has spelled nothing out yet; that is a fresh table, not a
			// hole, and the keys it sends next are its own.
			{
				NetSoundObservationDictionary warm;
				NetSoundObservationTables receiver;
				std::vector<uint8_t> bytes;
				size_t encoded = 0;
				if (!NetLockstepCodec::Encode({MakeObservationFrame(300, roundId, MakeObservationSet(2, 8, 21, 0.0F))}, bytes, nullptr, &warm, &encoded) ||
				    !NetLockstepCodec::Decode(bytes, ControllerFrame::c_Version, &receiver).ok) {
					*error = "the restart probe's first round did not survive the wire";
					return false;
				}
				NetSoundObservationDictionary restarted;
				const std::vector<NetSoundObservation> fresh = MakeObservationSet(2, 8, 44, 0.5F);
				std::vector<uint8_t> restartedBytes;
				if (!NetLockstepCodec::Encode({MakeObservationFrame(0, roundId + 1, fresh)}, restartedBytes, nullptr, &restarted, &encoded)) {
					*error = "the restarted round did not encode";
					return false;
				}
				const NetLockstepDecodeResult adopted = NetLockstepCodec::Decode(restartedBytes, ControllerFrame::c_Version, &receiver);
				const NetLockstepFrame* adoptedFrame = adopted.ok ? std::get_if<NetLockstepFrame>(&adopted.packet.payload) : nullptr;
				if (!adoptedFrame || adoptedFrame->observations != fresh) {
					*error = "a sender that started over was not followed: " + std::string(NetLockstepCodec::ErrorCodeName(adopted.error.code));
					return false;
				}
			}

			// Corruptions inside the block are refused, not read as another sound.
			NetSoundObservationDictionary roundEncoder;
			std::vector<uint8_t> roundBytes;
			size_t roundEncoded = 0;
			if (!NetLockstepCodec::Encode({MakeObservationFrame(60, roundId, MakeObservationSet(2, 3, 9, 0.0F))}, roundBytes, nullptr, &roundEncoder, &roundEncoded)) {
				*error = "could not encode the corruption frame";
				return false;
			}
			// Past the frame's empty block, which is a zero binding sequence and a zero count.
			const size_t blockStart = FrameBytesWithoutObservations(MakeObservationFrame(60, roundId, {}));
			const auto expectBlockError = [&](size_t offset, uint8_t value, NetLockstepErrorCode code, const char* what) {
				std::vector<uint8_t> corrupt = roundBytes;
				corrupt[offset] = value;
				NetSoundObservationTables tables;
				const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(corrupt, ControllerFrame::c_Version, &tables);
				if (decoded.error.code == code) {
					return true;
				}
				*error = std::string(what) + " gave " + NetLockstepCodec::ErrorCodeName(decoded.error.code);
				return false;
			};
			if (!expectBlockError(blockStart - 3, 0x05U, NetLockstepErrorCode::ObservationBindingGap, "a binding sequence ahead of the table") ||
			    !expectBlockError(blockStart, 0xFEU, NetLockstepErrorCode::UnboundObservationSlot, "a corrupted slot reference") ||
			    !expectBlockError(blockStart + 1, 0xE0U, NetLockstepErrorCode::ReservedFieldNonZero, "a reserved key-mask bit")) {
				return false;
			}
			std::vector<uint8_t> nonCanonical = roundBytes;
			nonCanonical[blockStart] = 0x81U; // A varint continuation whose next byte adds nothing.
			nonCanonical.insert(nonCanonical.begin() + static_cast<std::ptrdiff_t>(blockStart) + 1, 0x00U);
			for (int i = 0; i < 4; ++i) {
				nonCanonical[12 + i] = static_cast<uint8_t>((nonCanonical.size() - NetLockstepCodec::c_HeaderBytes) >> (i * 8));
			}
			NetSoundObservationTables varintTables;
			if (NetLockstepCodec::Decode(nonCanonical, ControllerFrame::c_Version, &varintTables).error.code != NetLockstepErrorCode::InvalidValue) {
				*error = "a non-canonical slot varint was not refused";
				return false;
			}

			// A version 14 frame has no binding sequence and still decodes, with and without a table.
			std::vector<uint8_t> version14 = roundBytes;
			if (version14[blockStart - 3] != 0x00U) {
				*error = "the corruption frame's binding sequence is not the single zero byte expected";
				return false;
			}
			version14.erase(version14.begin() + static_cast<std::ptrdiff_t>(blockStart) - 3);
			version14[4] = 14;
			for (int i = 0; i < 4; ++i) {
				version14[12 + i] = static_cast<uint8_t>((version14.size() - NetLockstepCodec::c_HeaderBytes) >> (i * 8));
			}
			for (NetSoundObservationTables* tables: {static_cast<NetSoundObservationTables*>(nullptr), &varintTables}) {
				const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(version14, ControllerFrame::c_Version, tables);
				const NetLockstepFrame* out = decoded.ok ? std::get_if<NetLockstepFrame>(&decoded.packet.payload) : nullptr;
				if (!out || out->observations != MakeObservationSet(2, 3, 9, 0.0F)) {
					*error = "a version 14 frame did not decode: " + std::string(NetLockstepCodec::ErrorCodeName(decoded.error.code));
					return false;
				}
			}
			std::cout << "[net-lockstep-selftest] PASS observation_slot_codec repeat=" << (repeatBytes - emptyBytes) / 64
			          << "B first_use=" << (firstBytes - emptyBytes) / 64 << "B legacy=44B budget_stop=" << floodEncoded << "/" << flood.size()
			          << " lost_rebinding=refused v14_control_committed_key=" << silentlyWrongKey << std::endl;
			return true;
		}

		bool TestRoundTrips(std::string* error) {
			const NetLockstepStart start{
				0xAABBCCDDEEFF0011ULL,
				30,
				2,
				ControllerFrame::c_Version,
				static_cast<uint16_t>(ControllerFrame::c_EncodedSize),
				1,
				2,
				"SimBaseline",
				"unique-id-split",
				0x5EED0000C0FFEE01ULL,
			};
			if (!RoundTrip({start}, error)) {
				return false;
			}

			NetLockstepFrame frame;
			frame.senderPeerId = 2;
			frame.targetFrame = 32;
			frame.roundId = 0x5EED0000C0FFEE01ULL;
			frame.observations = {NetSoundObservation{2, 1048601, 31, 0x1122334455667788ULL, 7, 3, 0.25F}, NetSoundObservation{2, 0, 12, 99, 0, 1, 0.75F}};
			frame.frames = {MakeFrame(100, 1), MakeFrame(200, 2)};
			frame.commands = {NetGameCommand{2, NetGameSetTeamFunds{0, 1500}}, NetGameCommand{2, NetGameSetTeamFunds{1, -250}}, NetGameCommand{2, NetGameSpawnActor{"AHuman", "Green Dummy", "Base.rte", 1234.5F, -67.25F, 1}}, NetGameCommand{2, NetGameDeliverCargo{"ACDropShip", "Dropship MK1", "Base.rte", 880.0F, 48.5F, 0, {{"AHuman", "Green Dummy", "Base.rte"}, {"AHuman", "Robot 1", "Base.rte"}}}}, NetGameCommand{2, NetGameDeliverCargo{"ACRocket", "Rocket MK2", "Base.rte", 512.0F, 300.0F, 1, {{"AHuman", "Green Dummy", "Base.rte"}}, true, 137.5F, false, 4, 600.0F, 350.25F, 424242, 1, -32.0F}}, NetGameCommand{2, NetGameScuttleCraft{17143, 0}}, NetGameCommand{2, NetGameInventoryOp{9001, 1, NetGameInventoryOp::Drop, 0, 2, true, 0.5F, -0.25F}}, NetGameCommand{2, NetGamePauseMatch{1, true}}, NetGameCommand{2, NetGamePauseMatch{0, false}}, NetGameCommand{2, NetGameSetActorAIMode{31337, 1, 6}}, NetGameCommand{2, NetGameSwitchControl{41414, 0, 2}}, NetGameCommand{2, NetGameAIEquip{51515, 1, NetGameAIEquip::LoadedFirearmInGroup, false, "Weapons - Primary", "Weapons - Explosive", "", ""}}, NetGameCommand{2, NetGameAIEquip{51516, 0, NetGameAIEquip::NamedDevice, false, "", "", "Base.rte", "Battle Rifle"}}, NetGameCommand{2, NetGameAIEquip{51517, 1, NetGameAIEquip::ShieldInBGArm, true, "", "", "", ""}}, NetGameCommand{2, NetGameAIEquip{51518, 0, NetGameAIEquip::UnequipFGArm, false, "", "", "", ""}}, NetGameCommand{2, NetGameAIOrder{61616, 0, NetGameAIOrder::FormSquad, 512.5F, -12.25F, 61617}}, NetGameCommand{2, NetGameAIOrder{61618, 1, NetGameAIOrder::MOWaypoint, 0.0F, 0.0F, 61616}}, NetGameCommand{2, NetGameSoundOp{71717, 1, 0x00FF00FF00FF0001ULL, NetGameSoundOp::Play, 0, 3, 0, 0.0F, 0.0F, {}, ""}}, NetGameCommand{2, NetGameSoundOp{71718, 0, 0x0000000000000002ULL, NetGameSoundOp::SetProperty, 13, -1, 0, -12.5F, 88.25F, {}, ""}}, NetGameCommand{2, NetGameSoundOp{71719, 1, 0x0000000000000003ULL, NetGameSoundOp::SelectSounds, 0, -1, 0, 0.0F, 0.0F, {2, 0, 7}, ""}}, NetGameCommand{2, NetGameSoundOp{71720, 0, 0x0000000000000004ULL, NetGameSoundOp::FadeOut, 0, -1, 250, 0.0F, 0.0F, {}, ""}}, NetGameCommand{2, NetGameSoundOp{71721, 1, 0x0000000000000005ULL, NetGameSoundOp::AddSound, 0, -1, 0, 0.0F, 0.0F, {1}, "9 SoundData1 31 Base.rte/Sounds/GUIs/Click.flac 0 0 0 3212836864 "}}, NetGameCommand{2, NetGameSoundOp{71722, 0, 0x0000000000000006ULL, NetGameSoundOp::SetCycleMode, 0, -1, 2, 0.0F, 0.0F, {}, ""}}};
			if (!RoundTrip({frame}, error)) {
				return false;
			}

			if (!RoundTrip({NetLockstepAck{1, 31, 0x0000FFFFU}}, error)) {
				return false;
			}
			if (!RoundTrip({NetLockstepStop{2, NetLockstepStopReason::Complete, 120, "done"}}, error)) {
				return false;
			}
			if (!RoundTrip({NetLockstepStop{3, NetLockstepStopReason::PeerLeft, 240, "left"}}, error)) {
				return false;
			}
			if (!RoundTrip({NetLockstepStop{3, NetLockstepStopReason::PeerDropped, 240, "connection lost"}}, error)) {
				return false;
			}
			if (!RoundTrip({NetLockstepStop{1, NetLockstepStopReason::ResyncRequested, 300, "rejoin"}}, error)) {
				return false;
			}
			if (!RoundTrip({NetLockstepStop{3, NetLockstepStopReason::Reclaimed, 240, "Reclaimed"}}, error)) {
				return false;
			}
			if (!RoundTrip({NetLockstepStop{3, NetLockstepStopReason::Substituted, 240, "Substituted"}}, error)) {
				return false;
			}
			if (!RoundTrip({NetLockstepStop{3, NetLockstepStopReason::Expired, 240, "Expired"}}, error)) {
				return false;
			}
			std::array<uint8_t, 32> checksumHash{};
			for (size_t i = 0; i < checksumHash.size(); ++i) {
				checksumHash[i] = static_cast<uint8_t>(i * 7 + 3);
			}
			if (!RoundTrip({NetLockstepChecksum{1, 99, checksumHash, 0x5EED0000C0FFEE01ULL}}, error)) {
				return false;
			}
			return true;
		}

		bool TestCanonicalHeader(std::string* error) {
			std::vector<uint8_t> bytes;
			if (!EncodePacket({NetLockstepAck{1, 0x1122334455667788ULL, 0xAABBCCDDU}}, bytes, error)) {
				return false;
			}
			if (bytes.size() != NetLockstepCodec::c_HeaderBytes + 16U) {
				*error = "canonical ack encoded size mismatch";
				return false;
			}
			const std::vector<uint8_t> expectedPrefix = {
				0x43, 0x43, 0x4C, 0x33,
				0x14, 0x00,
				0x10, 0x00,
				0x03, 0x00,
				0x00, 0x00,
				0x10, 0x00, 0x00, 0x00,
				0x01, 0x00, 0x00, 0x00,
			};
			for (size_t i = 0; i < expectedPrefix.size(); ++i) {
				if (bytes[i] != expectedPrefix[i]) {
					*error = "canonical header byte mismatch at " + std::to_string(i);
					return false;
				}
			}
			if (bytes[20] != 0x88U || bytes[27] != 0x11U || bytes[28] != 0xDDU || bytes[31] != 0xAAU) {
				*error = "canonical ack payload is not little-endian";
				return false;
			}
			return true;
		}

		bool TestDecodeFailures(std::string* error) {
			std::vector<uint8_t> bytes;
			if (!EncodePacket({NetLockstepAck{1, 2, 3}}, bytes, error)) {
				return false;
			}

			if (!ExpectDecodeError({}, NetLockstepErrorCode::ShortHeader, error)) {
				return false;
			}

			std::vector<uint8_t> mutated = bytes;
			mutated[0] = 0;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::BadMagic, error)) {
				return false;
			}

			mutated = bytes;
			mutated[4] = 0xFFU;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::UnsupportedVersion, error)) {
				return false;
			}

			mutated = bytes;
			mutated[6] = 0x18U;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::BadHeaderSize, error)) {
				return false;
			}

			mutated = bytes;
			mutated[8] = 0xFEU;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::UnknownPacketType, error)) {
				return false;
			}

			mutated = bytes;
			mutated[10] = 0x01U;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::UnknownFlags, error)) {
				return false;
			}

			mutated = bytes;
			mutated[12] = 0xFFU;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::PayloadLengthMismatch, error)) {
				return false;
			}

			mutated = bytes;
			mutated[12] = 0x01U;
			mutated[14] = 0x01U;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::PayloadTooLarge, error)) {
				return false;
			}

			mutated = bytes;
			mutated.push_back(0);
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::PayloadLengthMismatch, error)) {
				return false;
			}

			mutated = bytes;
			mutated.resize(NetLockstepCodec::c_HeaderBytes + 4U);
			mutated[12] = 0x04U;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::TruncatedPayload, error)) {
				return false;
			}

			mutated = bytes;
			mutated[NetLockstepCodec::c_HeaderBytes + 1] = 1U;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::ReservedFieldNonZero, error)) {
				return false;
			}
			return true;
		}

		bool TestSemanticFailures(std::string* error) {
			NetLockstepStart start{
				1,
				0,
				2,
				ControllerFrame::c_Version,
				static_cast<uint16_t>(ControllerFrame::c_EncodedSize),
				1,
				2,
				"bad\nscenario",
				"unique-id-split",
			};
			std::vector<uint8_t> bytes;
			NetLockstepError encodeError;
			if (NetLockstepCodec::Encode({start}, bytes, &encodeError) || encodeError.code != NetLockstepErrorCode::InvalidString) {
				*error = "expected invalid start string encode failure";
				return false;
			}

			start.scenario.assign(NetLockstepCodec::c_MaxScenarioBytes + 1, 'x');
			if (NetLockstepCodec::Encode({start}, bytes, &encodeError) || encodeError.code != NetLockstepErrorCode::StringTooLong) {
				*error = "expected oversized start string encode failure";
				return false;
			}

			start.scenario = "SimBaseline";
			start.controllerFrameVersion = ControllerFrame::c_Version + 1;
			if (NetLockstepCodec::Encode({start}, bytes, &encodeError) || encodeError.code != NetLockstepErrorCode::InvalidValue) {
				*error = "expected controller version encode failure";
				return false;
			}

			NetLockstepFrame frame;
			frame.senderPeerId = 1;
			frame.targetFrame = 10;
			frame.frames = {MakeFrame(200, 1), MakeFrame(100, 2)};
			if (NetLockstepCodec::Encode({frame}, bytes, &encodeError) || encodeError.code != NetLockstepErrorCode::InvalidValue) {
				*error = "expected unsorted frame encode failure";
				return false;
			}

			frame.frames = {MakeFrame(100, 1), MakeFrame(100, 2)};
			if (NetLockstepCodec::Encode({frame}, bytes, &encodeError) || encodeError.code != NetLockstepErrorCode::InvalidValue) {
				*error = "expected duplicate frame encode failure";
				return false;
			}

			frame.frames = {MakeFrame(100, 1)};
			frame.frames[0].flags = 0x80U;
			if (NetLockstepCodec::Encode({frame}, bytes, &encodeError) || encodeError.code != NetLockstepErrorCode::InvalidValue) {
				*error = "expected invalid ControllerFrame encode failure";
				return false;
			}

			if (!EncodePacket({NetLockstepStop{1, NetLockstepStopReason::InternalError, 0, "x"}}, bytes, error)) {
				return false;
			}
			std::vector<uint8_t> mutated = bytes;
			mutated[NetLockstepCodec::c_HeaderBytes + 2] = 0xFEU;
			mutated[NetLockstepCodec::c_HeaderBytes + 3] = 0xFFU;
			if (!ExpectDecodeError(mutated, NetLockstepErrorCode::InvalidValue, error)) {
				return false;
			}
			return true;
		}

		bool StartCoordinatorPair(uint16_t port, LoopbackTransport& hostTransport, LoopbackTransport& clientTransport, NetLockstepCoordinator& host, NetLockstepCoordinator& client, NetLockstepConfig hostConfig, NetLockstepConfig clientConfig, std::string* error) {
			if (!hostTransport.StartHost(port, error)) {
				return false;
			}
			if (!clientTransport.Connect("loopback", port, error)) {
				return false;
			}
			hostConfig.remoteTransportPeerId = 1;
			clientConfig.remoteTransportPeerId = 1;
			if (!host.Start(hostTransport, hostConfig, error)) {
				return false;
			}
			if (!client.Start(clientTransport, clientConfig, error)) {
				return false;
			}
			return true;
		}

		NetLockstepConfig MakeCoordinatorConfig(uint8_t localPeerId, uint8_t remotePeerId, uint64_t sessionId, uint16_t inputDelayFrames, NetTransportLane frameLane) {
			NetLockstepConfig config;
			config.sessionId = sessionId;
			config.startFrame = 0;
			config.inputDelayFrames = inputDelayFrames;
			config.timeoutMs = 250;
			config.localPeerId = localPeerId;
			config.remotePeerId = remotePeerId;
			config.peerCount = 2;
			config.frameLane = frameLane;
			config.scenario = "LockstepSelfTest";
			config.ownershipPolicy = "unique-id-split";
			return config;
		}

		bool TestSenderDropsUncontrolledTeamCommands(std::string* error) {
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			NetMatchConfig matchConfig = NetMatchConfigUtil::MakeDefault(0x5732315433414D00ULL);
			matchConfig.ownershipPolicy = NetActorOwnershipPolicy::TeamOwner;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, 43021, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, 43021, 0, NetTransportLane::ControlReliable);
			hostConfig.matchConfig = matchConfig;
			clientConfig.matchConfig = matchConfig;
			hostConfig.ownershipPolicy = "team-owner";
			clientConfig.ownershipPolicy = "team-owner";
			if (!StartCoordinatorPair(43021, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			ScenarioRunner::SetLockstepCoordinator(&host);
			ScenarioRunner::DrainLocalGameCommands();
			ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{1, NetGameAIOrder{1002, 1, NetGameAIOrder::PopWaypoint, 10.0F, 20.0F, 0}});
			if (!ScenarioRunner::DrainLocalGameCommands().empty()) {
				ScenarioRunner::SetLockstepCoordinator(nullptr);
				*error = "host enqueued an AIOrder for a team it does not control";
				return false;
			}
			ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{1, NetGameAIOrder{1001, 0, NetGameAIOrder::PopWaypoint, 10.0F, 20.0F, 0}});
			const std::vector<NetGameCommand> kept = ScenarioRunner::DrainLocalGameCommands();
			ScenarioRunner::SetLockstepCoordinator(nullptr);
			if (kept.size() != 1) {
				*error = "host dropped an AIOrder for the team it controls";
				return false;
			}
			return true;
		}

		void DrainReady(NetLockstepCoordinator& coordinator, std::vector<uint64_t>& readyFrames) {
			NetLockstepReadyFrame ready;
			while (coordinator.PopReadyFrame(ready)) {
				readyFrames.push_back(ready.frame);
			}
		}

		bool DriveCoordinators(LoopbackTransport& hostTransport, LoopbackTransport& clientTransport, NetLockstepCoordinator& host, NetLockstepCoordinator& client, const std::function<bool()>& done, std::string* error, uint64_t maxMs = 1000, uint64_t stepMs = 5) {
			for (uint64_t now = 0; now <= maxMs; now += stepMs) {
				host.Tick(now);
				client.Tick(now);
				if (done()) {
					return true;
				}
				hostTransport.AdvanceTimeMs(stepMs);
				clientTransport.AdvanceTimeMs(stepMs);
			}
			*error = "condition not reached; host=" + std::string(NetLockstepCoordinator::StateName(host.GetState())) +
			         " client=" + NetLockstepCoordinator::StateName(client.GetState()) +
			         " host_report=" + host.BuildReportJson();
			return false;
		}

		bool TestRecoveryStopsAtCompletedTick(std::string* error) {
			for (uint16_t delay: {0, 3}) {
				for (bool rejoin: {false, true}) {
					LoopbackTransport hostTransport, clientTransport;
					NetLockstepCoordinator host, client;
					const uint16_t port = 43920 + delay + (rejoin ? 10 : 0);
					if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client,
					    MakeCoordinatorConfig(1, 2, port, delay, NetTransportLane::ControlReliable),
					    MakeCoordinatorConfig(2, 1, port, delay, NetTransportLane::ControlReliable), error)) return false;
					host.DeferStopsToTickBoundary();
					client.DeferStopsToTickBoundary();
					if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error)) return false;
					if (!host.QueueLocalInput(0, {MakeFrame(100, 1)}, {}, error) || !client.QueueLocalInput(0, {MakeFrame(101, 1)}, {}, error)) return false;
					if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.GetStats().framesAccepted == 1 && client.GetStats().framesAccepted == 1; }, error)) return false;
					if (rejoin) {
						host.RequestResync("player rejoined");
					} else {
						std::array<uint8_t, 32> first{}, second{};
						second[0] = 1;
						if (!host.SubmitLocalChecksum(delay, first, error) || !client.SubmitLocalChecksum(delay, second, error)) return false;
					}
					if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.HasPendingRecoveryStop(); }, error)) return false;
					if (!host.IsRunning() || !client.IsRunning()) {
						*error = "recovery stopped input before the authoritative tick completed";
						return false;
					}
					if (!host.QueueLocalInput(1, {MakeFrame(100, 2)}, {}, error) || !client.QueueLocalInput(1, {MakeFrame(101, 2)}, {}, error)) return false;
					if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.GetStats().framesAccepted == 2 && client.GetStats().framesAccepted == 2; }, error)) return false;
					if (client.FinishSimulationTick(delay + 1) || !host.FinishSimulationTick(delay + 1) || !host.IsFailed()) {
						*error = "the host did not own the recovery boundary";
						return false;
					}
					if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return client.IsFailed(); }, error)) return false;
					const std::string reason = rejoin ? "ResyncRequested:" : "Desync:";
					if (!host.GetStats().timeoutReason.starts_with(reason) || !client.GetStats().timeoutReason.starts_with(reason)) {
						*error = "recovery lost its reason";
						return false;
					}
				}
			}
			std::cout << "[net-lockstep-selftest] PASS recovery_stops_at_completed_tick D=0,3 desync/rejoin" << std::endl;
			return true;
		}

		bool TestCompletionDrainsAppliedTicks(std::string* error) {
			for (uint16_t delay: {0, 3}) {
				LoopbackTransport hostTransport, clientTransport;
				NetLockstepCoordinator host, client;
				const uint16_t port = 43940 + delay;
				if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client,
				    MakeCoordinatorConfig(1, 2, port, delay, NetTransportLane::ControlReliable),
				    MakeCoordinatorConfig(2, 1, port, delay, NetTransportLane::ControlReliable), error)) return false;
				host.DeferStopsToTickBoundary();
				client.DeferStopsToTickBoundary();
				if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error)) return false;
				for (uint64_t produced = 0; produced < 2; ++produced) {
					if (!host.QueueLocalInput(produced, {MakeFrame(100, produced)}, {}, error) || !client.QueueLocalInput(produced, {MakeFrame(101, produced)}, {}, error)) return false;
				}
				if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.GetStats().framesAccepted == 2 && client.GetStats().framesAccepted == 2; }, error)) return false;
				const auto before = nlohmann::json::parse(host.BuildReportJson());
				if (!before["completed_simulation_tick"].is_null() || before["ready_frames_pending"] != 2) {
					*error = "prefetched frames were reported as completed simulation";
					return false;
				}
				host.FinishSimulationTick(delay);
				host.Complete("finished at applied tick");
				for (int poll = 0; poll < 10; ++poll) {
					hostTransport.AdvanceTimeMs(1);
					clientTransport.AdvanceTimeMs(1);
					client.Tick(clientTransport.NowMs());
				}
				if (!client.IsRunning()) {
					*error = "completion discarded an unapplied final simulation tick";
					return false;
				}
				NetLockstepReadyFrame finalFrame;
				if (!client.PopReadyFrame(finalFrame) || finalFrame.frame != delay || !client.FinishSimulationTick(delay) || !client.IsStopped()) {
					*error = "completion used prefetched input instead of the sender's completed simulation tick";
					return false;
				}
				if (client.GetStats().timeoutReason != "Complete:finished at applied tick") return false;
				const auto report = nlohmann::json::parse(client.BuildReportJson());
				if (report["completed_simulation_tick"] != delay || report["ready_frames_pending"] != 1 || report["frames_accepted"] != 2) {
					*error = "completion report lost its completed tick or prefetched tail";
					return false;
				}
			}
			std::cout << "[net-lockstep-selftest] PASS completion_drains_applied_ticks D=0,3" << std::endl;
			return true;
		}

		bool TestCoordinatorDelayedHappyPath(std::string* error) {
			const uint16_t port = 43001;
			const uint64_t sessionId = 0x7000000000000001ULL;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client,
			                          MakeCoordinatorConfig(1, 2, sessionId, 2, NetTransportLane::ControlReliable),
			                          MakeCoordinatorConfig(2, 1, sessionId, 2, NetTransportLane::ControlReliable), error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error)) {
				return false;
			}
			if (host.GetStats().effectiveStartFrame != 2 || client.GetStats().effectiveStartFrame != 2) {
				*error = "input-delay effective start frame was not applied";
				return false;
			}

			const NetGameCommand fundsCommand{1, NetGameSetTeamFunds{0, 4200}};
			for (uint64_t producedFrame = 0; producedFrame < 5; ++producedFrame) {
				const std::vector<NetGameCommand> hostCommands = producedFrame == 0 ? std::vector<NetGameCommand>{fundsCommand} : std::vector<NetGameCommand>{};
				if (!host.QueueLocalInput(producedFrame, {MakeFrame(100 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, hostCommands, error) ||
				    !client.QueueLocalInput(producedFrame, {MakeFrame(200 + static_cast<int64_t>(producedFrame), producedFrame + 11)}, {}, error)) {
					return false;
				}
			}

			std::vector<uint64_t> hostReady;
			std::vector<uint64_t> clientReady;
			std::vector<NetGameCommand> hostLocalCommands;
			std::vector<NetGameCommand> clientRemoteCommands;
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] {
					NetLockstepReadyFrame ready;
					while (host.PopReadyFrame(ready)) {
						hostReady.push_back(ready.frame);
						hostLocalCommands.insert(hostLocalCommands.end(), ready.localCommands.begin(), ready.localCommands.end());
					}
					while (client.PopReadyFrame(ready)) {
						clientReady.push_back(ready.frame);
						clientRemoteCommands.insert(clientRemoteCommands.end(), ready.remoteCommands.begin(), ready.remoteCommands.end());
					}
					return hostReady.size() == 5 && clientReady.size() == 5;
				}, error)) {
				return false;
			}
			if (hostReady.front() != 2 || clientReady.front() != 2 || host.GetStats().framesAccepted != 5 || client.GetStats().framesAccepted != 5) {
				*error = "delayed lockstep did not accept the expected ready frames";
				return false;
			}
			// The host's funds command must surface in its own ready frame (localCommands) and on the client (remoteCommands).
			if (hostLocalCommands.size() != 1 || !(hostLocalCommands.front() == fundsCommand) ||
			    clientRemoteCommands.size() != 1 || !(clientRemoteCommands.front() == fundsCommand)) {
				*error = "game command did not surface in the synced ready frame";
				return false;
			}
			const std::string report = host.BuildReportJson();
			if (report.find("\"input_delay_frames\":2") == std::string::npos ||
			    report.find("\"frames_accepted\":5") == std::string::npos ||
			    report.find("\"timeouts\":0") == std::string::npos) {
				*error = "happy-path report is missing deterministic stats";
				return false;
			}
			return true;
		}

		// A resync restarts lockstep while a peer is still reloading: the host's start reaches a
		// transport nobody is polling for it, and the host's first frames then arrive before the
		// peer ever sees a start. The frames must wait for the retransmitted start, not fail the round.
		bool TestCoordinatorFrameBeforeStart(std::string* error) {
			const uint16_t port = 43011;
			const uint64_t sessionId = 0x7000000000000011ULL;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			if (!hostTransport.StartHost(port, error) || !clientTransport.Connect("loopback", port, error)) {
				return false;
			}
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 2, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 2, NetTransportLane::ControlReliable);
			hostConfig.roundId = 0x9A5E000000000007ULL;
			hostConfig.remoteTransportPeerId = 1;
			clientConfig.remoteTransportPeerId = 1;
			// The missing-frame grace must outlast the start retransmit cycle, as a real match's does.
			hostConfig.timeoutMs = 2000;
			clientConfig.timeoutMs = 2000;
			// One clock for the whole scenario; a wait that spans two drives must not see time go backwards.
			uint64_t now = 0;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 10, now += 10) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					hostTransport.AdvanceTimeMs(10);
					clientTransport.AdvanceTimeMs(10);
				}
				*error = "condition not reached; host=" + host.BuildReportJson() + " client=" + client.BuildReportJson();
				return false;
			};
			if (!host.Start(hostTransport, hostConfig, error)) {
				return false;
			}
			// The client is between rounds: its transport drains the host's start into the void.
			clientTransport.AdvanceTimeMs(10);
			if (clientTransport.PollEvents().empty()) {
				*error = "the host start did not reach the client transport";
				return false;
			}
			if (!client.Start(clientTransport, clientConfig, error)) {
				return false;
			}
			if (!drive([&] { return host.IsRunning(); }, 100)) {
				return false;
			}
			for (uint64_t producedFrame = 0; producedFrame < 3; ++producedFrame) {
				if (!host.QueueLocalInput(producedFrame, {MakeFrame(100 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, {}, error)) {
					return false;
				}
			}
			if (!drive([&] { return client.GetStats().preStartFramesBuffered == 3; }, 100)) {
				return false;
			}
			if (client.IsFailed() || client.IsRunning()) {
				*error = "frames that outran the start were not held (" + client.BuildReportJson() + ")";
				return false;
			}
			if (!drive([&] { return client.IsRunning(); }, 1000)) {
				return false;
			}
			for (uint64_t producedFrame = 0; producedFrame < 3; ++producedFrame) {
				if (!client.QueueLocalInput(producedFrame, {MakeFrame(200 + static_cast<int64_t>(producedFrame), producedFrame + 11)}, {}, error)) {
					return false;
				}
			}
			std::vector<uint64_t> hostReady;
			std::vector<uint64_t> clientReady;
			if (!drive([&] {
					DrainReady(host, hostReady);
					DrainReady(client, clientReady);
					return hostReady.size() == 3 && clientReady.size() == 3;
				}, 1000)) {
				return false;
			}
			if (host.GetStats().startRetransmits == 0 || client.GetStats().startRetransmits == 0 || client.GetRoundId() != hostConfig.roundId) {
				*error = "the missed start was not repeated (" + host.BuildReportJson() + " / " + client.BuildReportJson() + ")";
				return false;
			}
			if (hostReady != std::vector<uint64_t>{2, 3, 4} || clientReady != std::vector<uint64_t>{2, 3, 4}) {
				*error = "held frames did not commit after the start";
				return false;
			}
			return true;
		}

		// A frame or checksum tagged with another round is a straggler from before a resync: ignored, never applied or failed on.
		bool TestCoordinatorIgnoresStaleRound(std::string* error) {
			const uint16_t port = 43012;
			const uint64_t sessionId = 0x7000000000000012ULL;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 2, NetTransportLane::ControlReliable);
			hostConfig.roundId = 0x9A5E000000000008ULL;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig,
			                          MakeCoordinatorConfig(2, 1, sessionId, 2, NetTransportLane::ControlReliable), error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error)) {
				return false;
			}
			if (client.GetRoundId() != hostConfig.roundId) {
				*error = "client did not adopt the host's round";
				return false;
			}
			NetLockstepFrame stale;
			stale.senderPeerId = 1;
			stale.targetFrame = 2;
			stale.roundId = 0x9A5E000000000001ULL;
			stale.frames = {MakeFrame(100, 1)};
			std::vector<uint8_t> bytes;
			if (!EncodePacket({stale}, bytes, error) || !hostTransport.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			NetLockstepChecksum staleChecksum;
			staleChecksum.senderPeerId = 1;
			staleChecksum.frame = 2;
			staleChecksum.roundId = 0x9A5E000000000001ULL;
			if (!EncodePacket({staleChecksum}, bytes, error) || !hostTransport.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return client.GetStats().staleRoundPackets == 2; }, error)) {
				return false;
			}
			if (client.IsFailed() || client.GetStats().remoteControllerFramesReceived != 0) {
				*error = "a stale-round packet was applied or failed the round";
				return false;
			}
			for (uint64_t producedFrame = 0; producedFrame < 3; ++producedFrame) {
				if (!host.QueueLocalInput(producedFrame, {MakeFrame(100 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, {}, error) ||
				    !client.QueueLocalInput(producedFrame, {MakeFrame(200 + static_cast<int64_t>(producedFrame), producedFrame + 11)}, {}, error)) {
					return false;
				}
			}
			std::vector<uint64_t> hostReady;
			std::vector<uint64_t> clientReady;
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] {
					DrainReady(host, hostReady);
					DrainReady(client, clientReady);
					return hostReady.size() == 3 && clientReady.size() == 3;
				}, error)) {
				return false;
			}
			return true;
		}

		// P8-1: each sender runs its OWN input delay. The commit stream starts at the earliest
		// sender's first delayed frame; slower senders ramp in, and every peer must build the
		// byte-identical per-frame apply set (local + remote union) through the ramp-in window.
		bool TestCoordinatorPerSenderDelay(std::string* error) {
			const uint16_t port = 43005;
			const uint64_t sessionId = 0x7000000000000005ULL;
			const std::map<uint8_t, uint16_t> delays = {{1, 1}, {2, 3}};

			// A local delay that disagrees with the per-peer set must be rejected up front.
			{
				LoopbackTransport rejectTransport;
				if (!rejectTransport.StartHost(43006, error)) {
					return false;
				}
				NetLockstepConfig bad = MakeCoordinatorConfig(1, 2, sessionId, 2, NetTransportLane::ControlReliable);
				bad.peerInputDelayFrames = delays;
				bad.remoteTransportPeerId = 1;
				NetLockstepCoordinator reject;
				std::string rejectError;
				if (reject.Start(rejectTransport, bad, &rejectError) || rejectError.find("disagrees") == std::string::npos) {
					*error = "mismatched local delay was not rejected at start";
					return false;
				}
			}

			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 1, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 3, NetTransportLane::ControlReliable);
			hostConfig.peerInputDelayFrames = delays;
			clientConfig.peerInputDelayFrames = delays;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error)) {
				return false;
			}
			if (host.GetStats().effectiveStartFrame != 1 || client.GetStats().effectiveStartFrame != 1) {
				*error = "per-sender commit stream did not start at the earliest sender's delay";
				return false;
			}

			for (uint64_t producedFrame = 0; producedFrame < 5; ++producedFrame) {
				if (!host.QueueLocalInput(producedFrame, {MakeFrame(100 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, {}, error) ||
				    !client.QueueLocalInput(producedFrame, {MakeFrame(200 + static_cast<int64_t>(producedFrame), producedFrame + 11)}, {}, error)) {
					return false;
				}
			}

			std::map<uint64_t, std::vector<int64_t>> hostUnions;
			std::map<uint64_t, std::vector<int64_t>> clientUnions;
			auto collectUnions = [](NetLockstepCoordinator& coordinator, std::map<uint64_t, std::vector<int64_t>>& out) {
				NetLockstepReadyFrame ready;
				while (coordinator.PopReadyFrame(ready)) {
					std::vector<int64_t> ids;
					for (const ControllerFrame& controllerFrame: ready.localFrames) {
						ids.push_back(controllerFrame.actorUniqueID);
					}
					for (const ControllerFrame& controllerFrame: ready.remoteFrames) {
						ids.push_back(controllerFrame.actorUniqueID);
					}
					std::sort(ids.begin(), ids.end());
					out[ready.frame] = std::move(ids);
				}
			};
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] {
					collectUnions(host, hostUnions);
					collectUnions(client, clientUnions);
					return hostUnions.size() >= 5 && clientUnions.size() >= 5;
				}, error)) {
				return false;
			}
			// Frames 1-2 carry the host alone (the client is still inside its delay); 3-5 carry both.
			if (hostUnions.begin()->first != 1 || clientUnions.begin()->first != 1 || hostUnions != clientUnions) {
				*error = "per-sender ramp-in did not produce identical apply sets on both peers";
				return false;
			}
			if (hostUnions[1] != std::vector<int64_t>{100} || hostUnions[3] != std::vector<int64_t>{102, 200}) {
				*error = "per-sender ramp-in merged the wrong sender sets";
				return false;
			}
			if (host.BuildReportJson().find("\"peer_input_delays\":{\"1\":1,\"2\":3}") == std::string::npos) {
				*error = "per-peer input delays are missing from the report";
				return false;
			}
			return true;
		}

		// A peer whose announced delay disagrees with the shared per-peer set must fail the start.
		bool TestCoordinatorPerSenderDelayMismatch(std::string* error) {
			const uint16_t port = 43007;
			const uint64_t sessionId = 0x7000000000000007ULL;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 1, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 2, NetTransportLane::ControlReliable);
			hostConfig.peerInputDelayFrames = {{1, 1}, {2, 3}};
			clientConfig.peerInputDelayFrames = {{1, 1}, {2, 2}};
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsFailed(); }, error)) {
				return false;
			}
			if (host.GetStats().timeoutReason.find("start mismatch") == std::string::npos) {
				*error = "per-sender delay mismatch did not fail the start";
				return false;
			}
			return true;
		}

		bool TestCoordinatorIgnoresSessionPacketsAtHandoff(std::string* error) {
			const uint16_t port = 43004;
			const uint64_t sessionId = 0x7000000000000004ULL;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			if (!hostTransport.StartHost(port, error) || !clientTransport.Connect("loopback", port, error)) {
				return false;
			}

			std::vector<uint8_t> heartbeatBytes;
			NetProtocolError protocolError;
			if (!NetProtocol::Encode({99, 0, NetHeartbeat{10, 2, 3}}, heartbeatBytes, &protocolError)) {
				*error = "session heartbeat encode failed: " + protocolError.message;
				return false;
			}
			if (!hostTransport.Send(1, NetTransportLane::ControlReliable, heartbeatBytes, error) ||
			    !clientTransport.Send(1, NetTransportLane::ControlReliable, heartbeatBytes, error)) {
				return false;
			}

			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::ControlReliable);
			hostConfig.remoteTransportPeerId = 1;
			clientConfig.remoteTransportPeerId = 1;
			if (!host.Start(hostTransport, hostConfig, error) || !client.Start(clientTransport, clientConfig, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error)) {
				return false;
			}
			if (host.GetStats().ignoredSessionPackets == 0 || client.GetStats().ignoredSessionPackets == 0) {
				*error = "coordinator did not report ignored session packets at handoff";
				return false;
			}
			return true;
		}

		bool TestCoordinatorUnreliableOutOfOrderDuplicate(std::string* error) {
			const uint16_t port = 43002;
			const uint64_t sessionId = 0x7000000000000002ULL;
			LoopbackTransportConfig faults;
			faults.latencyMs = 2;
			faults.jitterMs = 5;
			faults.reorderUnreliable = true;
			faults.unreliableDuplicateEveryN = 2;

			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			hostTransport.SetFaultConfig(faults);
			clientTransport.SetFaultConfig(faults);
			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client,
			                          MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::InputUnreliable),
			                          MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::InputUnreliable), error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error)) {
				return false;
			}

			for (uint64_t producedFrame : {1ULL, 0ULL, 2ULL, 3ULL}) {
				if (!host.QueueLocalInput(producedFrame, {MakeFrame(100 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, {}, error) ||
				    !client.QueueLocalInput(producedFrame, {MakeFrame(200 + static_cast<int64_t>(producedFrame), producedFrame + 11)}, {}, error)) {
					return false;
				}
			}

			std::vector<uint64_t> hostReady;
			std::vector<uint64_t> clientReady;
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] {
					DrainReady(host, hostReady);
					DrainReady(client, clientReady);
					return hostReady.size() == 4 && clientReady.size() == 4 &&
					       host.GetStats().duplicateFrames > 0 && client.GetStats().duplicateFrames > 0;
				}, error, 1500)) {
				return false;
			}
			if (hostReady != std::vector<uint64_t>{0, 1, 2, 3} || clientReady != std::vector<uint64_t>{0, 1, 2, 3}) {
				*error = "out-of-order frames were not released in frame order";
				return false;
			}
			if (host.GetStats().outOfOrderFrames == 0 || client.GetStats().outOfOrderFrames == 0) {
				*error = "out-of-order frame handling was not exercised";
				return false;
			}
			return true;
		}

		bool TestCoordinatorMissingFrameTimeout(std::string* error) {
			const uint16_t port = 43003;
			const uint64_t sessionId = 0x7000000000000003ULL;
			LoopbackTransportConfig clientFaults;
			clientFaults.unreliableDropEveryN = 2;

			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			clientTransport.SetFaultConfig(clientFaults);
			NetLockstepCoordinator host;
			NetLockstepCoordinator client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::InputUnreliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::InputUnreliable);
			hostConfig.timeoutMs = 40;
			clientConfig.timeoutMs = 40;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error)) {
				return false;
			}
			if (!host.QueueLocalInput(0, {MakeFrame(100, 1)}, {}, error) ||
			    !client.QueueLocalInput(0, {MakeFrame(200, 2)}, {}, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsFailed(); }, error, 500)) {
				return false;
			}
			if (host.GetStats().missingFrameStalls == 0 ||
			    host.GetStats().timeoutReason.find("MissingFrameTimeout") == std::string::npos ||
			    host.BuildReportJson().find("\"timeouts\":1") == std::string::npos) {
				*error = "missing-frame timeout did not produce the expected report";
				return false;
			}
			return true;
		}


		// P4C: three peers over a host-star loopback (clients connect only to the host, which relays).
		// Proves N-peer frame collection (advance only when all remotes are in), the peerId-ordered
		// merge, all-starts-before-run, and N-way checksum agreement.
		bool TestCoordinatorThreePeer(std::string* error) {
			const uint16_t port = 43010;
			const uint64_t sessionId = 0x7000000000000010ULL;
			LoopbackTransport hostT, clientAT, clientBT;
			if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) || !clientBT.Connect("loopback", port, error)) {
				return false;
			}
			// Client A connected first (host-side transport id 1), client B second (id 2). Clients see
			// the host as transport id 1. Peers: host=1, clientA=2, clientB=3.
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.startFrame = 0;
				c.inputDelayFrames = 0;
				c.timeoutMs = 500;
				c.localPeerId = local;
				c.peerCount = 3;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.frameLane = NetTransportLane::ControlReliable;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			NetLockstepCoordinator host, clientA, clientB;
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error) ||
			    !clientA.Start(clientAT, cfg(2, {{1, 1}}, false), error) ||
			    !clientB.Start(clientBT, cfg(3, {{1, 1}}, false), error)) {
				return false;
			}
			auto drive = [&](const std::function<bool()>& done) {
				for (uint64_t now = 0; now <= 2000; now += 5) {
					host.Tick(now);
					clientA.Tick(now);
					clientB.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					clientAT.AdvanceTimeMs(5);
					clientBT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && clientA.IsRunning() && clientB.IsRunning(); })) {
				*error = "three-peer lockstep did not reach Running (start relay failed)";
				return false;
			}
			for (uint64_t f = 0; f < 4; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100 + static_cast<int64_t>(f), f + 1)}, {}, error) ||
				    !clientA.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error) ||
				    !clientB.QueueLocalInput(f, {MakeFrame(300 + static_cast<int64_t>(f), f + 1)}, {}, error)) {
					return false;
				}
			}
			std::vector<uint64_t> hostReady, aReady, bReady;
			auto collect = [&](NetLockstepCoordinator& c, std::vector<uint64_t>& out, size_t& mergedRemotes) {
				NetLockstepReadyFrame ready;
				while (c.PopReadyFrame(ready)) {
					out.push_back(ready.frame);
					mergedRemotes = ready.remoteFrames.size();
				}
			};
			size_t hostRemotes = 0, aRemotes = 0, bRemotes = 0;
			if (!drive([&] {
					collect(host, hostReady, hostRemotes);
					collect(clientA, aReady, aRemotes);
					collect(clientB, bReady, bRemotes);
					return hostReady.size() >= 4 && aReady.size() >= 4 && bReady.size() >= 4;
				})) {
				*error = "three-peer lockstep did not produce 4 ready frames on every peer";
				return false;
			}
			// Every peer merges the two OTHER peers' frames into remoteFrames each tick.
			if (hostRemotes != 2 || aRemotes != 2 || bRemotes != 2) {
				*error = "three-peer ready frame did not merge both remote peers";
				return false;
			}
			// N-way checksum: all three agree on frame 0 -> verified, no desync.
			std::array<uint8_t, 32> hash{};
			hash.fill(0x5A);
			if (!host.SubmitLocalChecksum(0, hash, error) || !clientA.SubmitLocalChecksum(0, hash, error) || !clientB.SubmitLocalChecksum(0, hash, error)) {
				return false;
			}
			drive([&] { return false; });
			if (host.IsFailed() || clientA.IsFailed() || clientB.IsFailed()) {
				*error = "three-peer matching checksums wrongly desynced";
				return false;
			}
			return true;
		}

		bool TestCoordinatorSeatlessRelayHost(std::string* error) {
			const uint16_t port = 43017;
			const uint64_t sessionId = 0x7000000000000017ULL;
			LoopbackTransport hostT, clientAT, clientBT;
			if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) || !clientBT.Connect("loopback", port, error)) {
				return false;
			}
			// Peers: dedicated host=1 (no roster slot), clientA=2, clientB=3.
			NetMatchConfig matchConfig = NetMatchConfigUtil::MakeDefault(sessionId);
			matchConfig.peerCount = 3;
			matchConfig.dedicated = true;
			matchConfig.players = {
			    NetMatchPlayerSlot{2, 0, false, "Client A"},
			    NetMatchPlayerSlot{3, 1, false, "Client B"},
			};
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.startFrame = 0;
				c.inputDelayFrames = 0;
				c.timeoutMs = 500;
				c.localPeerId = local;
				c.peerCount = 3;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.frameLane = NetTransportLane::ControlReliable;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				c.matchConfig = matchConfig;
				return c;
			};
			NetLockstepCoordinator host, clientA, clientB;
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error) ||
			    !clientA.Start(clientAT, cfg(2, {{1, 1}}, false), error) ||
			    !clientB.Start(clientBT, cfg(3, {{1, 1}}, false), error)) {
				return false;
			}
			auto drive = [&](const std::function<bool()>& done) {
				for (uint64_t now = 0; now <= 2000; now += 5) {
					host.Tick(now);
					clientA.Tick(now);
					clientB.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					clientAT.AdvanceTimeMs(5);
					clientBT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && clientA.IsRunning() && clientB.IsRunning(); })) {
				*error = "seatless-host lockstep did not reach Running";
				return false;
			}
			// The seatless host still feeds the gate a packet every tick: empty frames plus its
			// bindings command; both remote gates and its own local gate require it.
			const NetGameCommand bindings{1, NetGamePlayerBindings{}, 0};
			for (uint64_t f = 0; f < 4; ++f) {
				if (!host.QueueLocalInput(f, {}, {bindings}, error) ||
				    !clientA.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error) ||
				    !clientB.QueueLocalInput(f, {MakeFrame(300 + static_cast<int64_t>(f), f + 1)}, {}, error)) {
					return false;
				}
			}
			struct Seen {
				std::vector<uint64_t> frames;
				std::map<uint8_t, size_t> remoteCounts;
				std::vector<int64_t> actors;
				std::vector<NetGameCommand> commands;
			};
			auto collect = [](NetLockstepCoordinator& c, Seen& out) {
				NetLockstepReadyFrame ready;
				while (c.PopReadyFrame(ready)) {
					out.frames.push_back(ready.frame);
					for (const auto& [peer, count] : ready.remoteFrameCounts) {
						out.remoteCounts[peer] += count;
					}
					for (const ControllerFrame& frame : ready.localFrames) out.actors.push_back(frame.actorUniqueID);
					for (const ControllerFrame& frame : ready.remoteFrames) out.actors.push_back(frame.actorUniqueID);
					out.commands.insert(out.commands.end(), ready.localCommands.begin(), ready.localCommands.end());
					out.commands.insert(out.commands.end(), ready.remoteCommands.begin(), ready.remoteCommands.end());
				}
			};
			Seen hostSeen, aSeen, bSeen;
			if (!drive([&] {
					collect(host, hostSeen);
					collect(clientA, aSeen);
					collect(clientB, bSeen);
					return hostSeen.frames.size() >= 4 && aSeen.frames.size() >= 4 && bSeen.frames.size() >= 4;
				})) {
				*error = "seatless-host lockstep did not commit 4 ready frames on every peer";
				return false;
			}
			// Local-vs-remote perspective reorders the merged stream, so compare the committed sets.
			auto sorted = [](Seen& seen) {
				std::sort(seen.actors.begin(), seen.actors.end());
				return true;
			};
			sorted(hostSeen);
			sorted(aSeen);
			sorted(bSeen);
			if (hostSeen.frames != aSeen.frames || aSeen.frames != bSeen.frames ||
			    hostSeen.actors != aSeen.actors || aSeen.actors != bSeen.actors ||
			    hostSeen.commands != aSeen.commands || aSeen.commands != bSeen.commands) {
				*error = "seatless-host peers did not commit identical ready frames";
				return false;
			}
			if (aSeen.remoteCounts[1] != 0 || bSeen.remoteCounts[1] != 0 || aSeen.actors.size() != 8) {
				*error = "the seatless host contributed controller frames to a committed tick";
				return false;
			}
			if (aSeen.commands.size() != 4 || bSeen.commands.size() != 4) {
				*error = "the seatless host's bindings command did not reach every tick";
				return false;
			}
			// A client leave keeps the gate correct: survivors advance without peer 3's packets.
			clientB.Leave("bye");
			for (uint64_t f = 4; f < 6; ++f) {
				if (!host.QueueLocalInput(f, {}, {bindings}, error) ||
				    !clientA.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error)) {
					return false;
				}
			}
			if (!drive([&] {
					collect(host, hostSeen);
					collect(clientA, aSeen);
					return hostSeen.frames.size() >= 6 && aSeen.frames.size() >= 6;
				})) {
				*error = "seatless-host survivors did not advance past the leaver";
				return false;
			}
			if (host.IsFailed() || clientA.IsFailed() ||
			    host.GetPeerLeaveFrames().count(3) == 0 || clientA.GetPeerLeaveFrames().count(3) == 0) {
				*error = "seatless-host survivors failed or did not record the leaver";
				return false;
			}
			if (aSeen.remoteCounts[3] != 4 || aSeen.remoteCounts[1] != 0) {
				*error = "the seatless-host gate merged the wrong remote set around the leave";
				return false;
			}
			return true;
		}

		bool TestCoordinatorRejectsUnboundPackets(std::string* error) {
			// Exercise the receive path, including its liveness bookkeeping, with every packet kind.
			// A departed seat remains a known logical peer but no longer owns any transport.
			std::string failures;
			for (int route = 0; route < 3; ++route) {
				for (int kind = 0; kind < 5; ++kind) {
					const uint16_t port = static_cast<uint16_t>(43940 + route * 5 + kind);
					LoopbackTransport hostT, clientAT, clientBT, strangerT;
					if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) ||
					    !clientBT.Connect("loopback", port, error)) return false;
					auto cfg = [](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
						NetLockstepConfig c;
						c.sessionId = 0x7000000000000090ULL;
						c.localPeerId = local;
						c.peerCount = 3;
						c.timeoutMs = 5000;
						c.roundId = 901;
						c.remoteTransportPeerIds = std::move(transports);
						c.relayToOtherPeers = relay;
						c.frameLane = NetTransportLane::ControlReliable;
						c.scenario = "LockstepSelfTest";
						c.ownershipPolicy = "unique-id-split";
						return c;
					};
					const auto hostConfig = cfg(1, {{2, 1}, {3, 2}}, true);
					NetLockstepCoordinator host, clientA, clientB;
					if (!host.Start(hostT, hostConfig, error) ||
					    !clientA.Start(clientAT, cfg(2, {{1, 1}}, false), error) ||
					    !clientB.Start(clientBT, cfg(3, {{1, 1}}, false), error)) return false;
					uint64_t now = 0;
					bool pumpB = true;
					auto pump = [&](unsigned steps) {
						for (unsigned step = 0; step < steps; ++step) {
							now += 5;
							hostT.AdvanceTimeMs(5);
							clientAT.AdvanceTimeMs(5);
							clientBT.AdvanceTimeMs(5);
							strangerT.AdvanceTimeMs(5);
							host.Tick(now);
							clientA.Tick(now);
							if (pumpB) clientB.Tick(now);
						}
					};
					pump(10);
					if (!host.IsRunning() || !clientA.IsRunning() || !clientB.IsRunning() ||
					    !host.QueueLocalInput(0, {MakeFrame(100, 1)}, {}, error) ||
					    !clientA.QueueLocalInput(0, {MakeFrame(200, 1)}, {}, error) ||
					    !clientB.QueueLocalInput(0, {MakeFrame(300, 1)}, {}, error)) return false;
					pump(10);
					NetLockstepReadyFrame ready;
					if (!host.PopReadyFrame(ready) || ready.frame != 0 || ready.remoteFrames.size() != 2 ||
					    !clientA.PopReadyFrame(ready) || ready.frame != 0 || ready.remoteFrames.size() != 2 ||
					    !clientB.PopReadyFrame(ready) || ready.frame != 0 || ready.remoteFrames.size() != 2) {
						*error = "transport-authority fixture did not commit its three-peer control frame";
						return false;
					}
					std::array<uint8_t, 32> goodHash{};
					goodHash.fill(0x5A);
					if (!host.SubmitLocalChecksum(0, goodHash, error) || !clientA.SubmitLocalChecksum(0, goodHash, error) ||
					    !clientB.SubmitLocalChecksum(0, goodHash, error)) return false;
					pump(10);
					if (!host.IsRunning() || !clientA.IsRunning() || !clientB.IsRunning()) {
						*error = "transport-authority fixture rejected legitimate relayed checksums";
						return false;
					}
					pumpB = false;
					if (route == 2) {
						clientBT.Stop();
						pump(10);
						if (host.GetPeerLeaveFrames().count(3) != 1 || clientA.GetPeerLeaveFrames().count(3) != 1 ||
						    host.UsesTransportPeer(2)) {
							*error = "transport-authority fixture did not remove the departed peer's binding";
							return false;
						}
					}
					if (route != 0 && !strangerT.Connect("loopback", port, error)) return false;
					pump(10);
					const uint64_t heardBefore = host.GetStats().peers.at(3).lastHeardMs;
					const auto hostStatsBefore = host.GetStats();
					const auto survivorStatsBefore = clientA.GetStats();
					if (!host.SubmitLocalChecksum(1, goodHash, error)) return false;
					NetLockstepPacket packet;
					if (kind == 0) {
						NetLockstepStart start;
						start.sessionId = hostConfig.sessionId;
						start.localPeerId = 3;
						start.peerCount = 3;
						start.controllerFrameVersion = ControllerFrame::c_Version;
						start.controllerFrameEncodedSize = static_cast<uint16_t>(ControllerFrame::c_EncodedSize);
						start.roundId = hostConfig.roundId;
						start.scenario = "DifferentScenario";
						start.ownershipPolicy = hostConfig.ownershipPolicy;
						packet.payload = start;
					} else if (kind == 1) {
						NetLockstepFrame frame;
						frame.senderPeerId = 3;
						frame.targetFrame = 1;
						frame.roundId = hostConfig.roundId;
						frame.frames = {MakeFrame(300, 2)};
						packet.payload = frame;
					} else if (kind == 2) {
						packet.payload = NetLockstepAck{3, 0, 0};
					} else if (kind == 3) {
						packet.payload = NetLockstepStop{3, NetLockstepStopReason::Desync, 1, "forged stop"};
					} else {
						NetLockstepChecksum checksum;
						checksum.senderPeerId = 3;
						checksum.frame = 1;
						checksum.roundId = hostConfig.roundId;
						checksum.hash.fill(0xA5);
						packet.payload = checksum;
					}
					std::vector<uint8_t> bytes;
					NetLockstepError codecError;
					if (!NetLockstepCodec::Encode(packet, bytes, &codecError)) {
						*error = codecError.message;
						return false;
					}
					LoopbackTransport& sender = route == 0 ? clientAT : strangerT;
					if (!sender.Send(1, NetTransportLane::ControlReliable, bytes, error)) return false;
					pump(10);
					if (!host.IsRunning() || !clientA.IsRunning() || host.GetStats().peers.at(3).lastHeardMs != heardBefore ||
					    host.GetStats().relayPacketsSent != hostStatsBefore.relayPacketsSent ||
					    host.GetStats().remoteControllerFramesReceived != hostStatsBefore.remoteControllerFramesReceived ||
					    clientA.GetStats().remoteControllerFramesReceived != survivorStatsBefore.remoteControllerFramesReceived) {
						failures += " route=" + std::to_string(route) + " kind=" + std::to_string(kind) +
						            " host=" + host.GetStats().timeoutReason + " survivor=" + clientA.GetStats().timeoutReason + ";";
						continue;
					}
					// A real peer's divergence must still be detected after the forged packet was rejected.
					std::array<uint8_t, 32> badHash{};
					badHash.fill(0xA5);
					if (!clientA.SubmitLocalChecksum(1, badHash, error)) return false;
					pump(10);
					if (!host.IsFailed() || host.GetStats().timeoutReason.find("sim state diverged at tick 1") == std::string::npos) {
						*error = "transport-authority fixture ignored a divergent checksum from its bound peer";
						return false;
					}
				}
			}
			if (!failures.empty()) {
				*error = "unauthorized packets changed the round or refreshed a claimed peer:" + failures;
				return false;
			}
			return true;
		}

		bool TestCoordinatorPeerLeave(std::string* error) {
			const uint16_t port = 43011;
			const uint64_t sessionId = 0x7000000000000011ULL;
			LoopbackTransport hostT, clientAT, clientBT;
			if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) || !clientBT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.timeoutMs = 500;
				c.localPeerId = local;
				c.peerCount = 3;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			NetLockstepCoordinator host, clientA, clientB;
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error) ||
			    !clientA.Start(clientAT, cfg(2, {{1, 1}}, false), error) ||
			    !clientB.Start(clientBT, cfg(3, {{1, 1}}, false), error)) {
				return false;
			}
			auto drive = [&](const std::function<bool()>& done) {
				for (uint64_t now = 0; now <= 2000; now += 5) {
					host.Tick(now);
					clientA.Tick(now);
					clientB.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					clientAT.AdvanceTimeMs(5);
					clientBT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && clientA.IsRunning() && clientB.IsRunning(); })) {
				*error = "leave test did not reach Running";
				return false;
			}
			// Everyone produces frames 0-1, then B leaves cleanly.
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !clientA.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error) ||
				    !clientB.QueueLocalInput(f, {MakeFrame(300, f + 1)}, {}, error)) {
					return false;
				}
			}
			clientB.Leave("bye");
			if (!clientB.IsStopped()) {
				*error = "leaver did not stop after Leave";
				return false;
			}
			// The survivors keep producing; frames 2-3 must advance WITHOUT B.
			for (uint64_t f = 2; f < 4; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !clientA.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error)) {
					return false;
				}
			}
			std::map<uint64_t, size_t> hostRemotesByFrame, aRemotesByFrame;
			auto collect = [](NetLockstepCoordinator& c, std::map<uint64_t, size_t>& out) {
				NetLockstepReadyFrame ready;
				while (c.PopReadyFrame(ready)) {
					out[ready.frame] = ready.remoteFrames.size();
				}
			};
			if (!drive([&] {
					collect(host, hostRemotesByFrame);
					collect(clientA, aRemotesByFrame);
					return hostRemotesByFrame.size() >= 4 && aRemotesByFrame.size() >= 4;
				})) {
				*error = "survivors did not advance past the leaver (host=" + std::to_string(hostRemotesByFrame.size()) +
				         " a=" + std::to_string(aRemotesByFrame.size()) + ")";
				return false;
			}
			if (host.IsFailed() || clientA.IsFailed()) {
				*error = "a survivor failed after a clean leave";
				return false;
			}
			// Frames through the leaver's last one carry its data; later frames drop to one remote.
			if (hostRemotesByFrame[1] != 2 || hostRemotesByFrame[2] != 1 || aRemotesByFrame[1] != 2 || aRemotesByFrame[2] != 1) {
				*error = "leave boundary merged the wrong remote sets";
				return false;
			}
			if (host.GetPeerLeaveFrames().count(3) == 0 || clientA.GetPeerLeaveFrames().count(3) == 0) {
				*error = "survivors did not record the leaver";
				return false;
			}
			// A 2-peer leave ends the peer's match: with B gone, A leaving leaves the host alone.
			clientA.Leave("bye too");
			drive([&] { return host.IsStopped(); });
			if (!host.IsStopped()) {
				*error = "host did not stop after every peer left";
				return false;
			}
			return true;
		}

		// A host-star fixture on the injected clock: host=1, clientA=2, clientB=3 over loopback.
		struct StarFixture {
			LoopbackTransport hostT, clientAT, clientBT;
			NetLockstepCoordinator host, clientA, clientB;
			uint64_t now = 0;
			uint64_t produced[3] = {0, 0, 0};

			bool Start(uint16_t port, uint64_t sessionId, uint32_t timeoutMs, std::string* error) {
				if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) || !clientBT.Connect("loopback", port, error)) {
					return false;
				}
				auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
					NetLockstepConfig c;
					c.sessionId = sessionId;
					c.timeoutMs = timeoutMs;
					c.localPeerId = local;
					c.peerCount = 3;
					c.remoteTransportPeerIds = std::move(transports);
					c.relayToOtherPeers = relay;
					c.scenario = "LockstepSelfTest";
					c.ownershipPolicy = "unique-id-split";
					return c;
				};
				return host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error) &&
				       clientA.Start(clientAT, cfg(2, {{1, 1}}, false), error) &&
				       clientB.Start(clientBT, cfg(3, {{1, 1}}, false), error);
			}

			// Drives only the named peers, so a peer left out is one whose process has wedged: its
			// socket stays open and its packets stop.
			bool Drive(bool driveHost, bool driveA, bool driveB, const std::function<bool()>& done, uint64_t untilMs) {
				for (; now <= untilMs; now += 5) {
					if (driveHost) host.Tick(now);
					if (driveA) clientA.Tick(now);
					if (driveB) clientB.Tick(now);
					if (done()) {
						return true;
					}
					if (driveHost) hostT.AdvanceTimeMs(5);
					if (driveA) clientAT.AdvanceTimeMs(5);
					if (driveB) clientBT.AdvanceTimeMs(5);
				}
				return false;
			}

			// Produces while this peer's pipeline has room, and stops when it does not: a peer blocked
			// on a frame it cannot commit stops sending, exactly as the sim loop does while it waits.
			void Feed(NetLockstepCoordinator& peer, uint8_t peerId, int64_t actorId) {
				uint64_t& next = produced[peerId - 1];
				if (!peer.IsRunning() || next > peer.GetStats().nextFrame + 4) {
					return;
				}
				std::string ignored;
				if (peer.QueueLocalInput(next, {MakeFrame(actorId, next + 1)}, {}, &ignored)) {
					++next;
				}
			}

			// Every live peer keeps producing, as a real match does. Without this the injected clock
			// runs on while nobody sends, and a peer that has simply run out of scripted input reads
			// as wedged - which is the very thing these tests have to tell apart.
			bool DriveProducing(bool driveB, const std::function<bool()>& done, uint64_t untilMs) {
				return Drive(true, true, driveB, [&] {
					Feed(host, 1, 100);
					Feed(clientA, 2, 200);
					if (driveB) {
						Feed(clientB, 3, 300);
					}
					return done();
				}, untilMs);
			}

			void Collect(NetLockstepCoordinator& peer, std::vector<uint64_t>& out) {
				NetLockstepReadyFrame ready;
				while (peer.PopReadyFrame(ready)) {
					out.push_back(ready.frame);
				}
			}

			bool Running() { return host.IsRunning() && clientA.IsRunning() && clientB.IsRunning(); }
		};

		// A wedged client stops sending but keeps its socket, so the transport says nothing for as long
		// as its process lives - longer than every survivor's missing-frame grace. The relay host must
		// call it gone on its OWN bounded budget, or the healthy clients time out waiting for a peer
		// nobody has told them about. Only the host adjudicates: one relayed notice, one leave frame.
		bool TestCoordinatorHostAdjudicatesSilentPeer(std::string* error) {
			StarFixture fx;
			const uint32_t timeoutMs = 400;
			if (!fx.Start(43012, 0x7000000000000012ULL, timeoutMs, error)) {
				return false;
			}
			if (!fx.Drive(true, true, true, [&] { return fx.Running(); }, 2000)) {
				*error = "silent-peer fixture did not reach Running";
				return false;
			}
			std::vector<uint64_t> hostReady, aReady;
			if (!fx.DriveProducing(true, [&] {
					fx.Collect(fx.host, hostReady);
					fx.Collect(fx.clientA, aReady);
					return hostReady.size() >= 2 && aReady.size() >= 2;
				}, fx.now + timeoutMs)) {
				*error = "silent-peer fixture did not get the round moving";
				return false;
			}
			// B wedges here: never ticked again, never sends again. Host and A keep playing.
			const uint64_t silentFrom = fx.now;
			if (!fx.DriveProducing(false, [&] { return fx.clientA.GetPeerLeaveFrames().count(3) != 0; }, silentFrom + 4 * timeoutMs)) {
				*error = "the survivor never learned the wedged peer had left (host=" + fx.host.BuildReportJson() + ")";
				return false;
			}
			if (fx.now - silentFrom >= timeoutMs) {
				*error = "the drop notice reached the survivor after its own grace (" + std::to_string(fx.now - silentFrom) +
				         "ms of " + std::to_string(timeoutMs) + "ms)";
				return false;
			}
			if (fx.host.GetStats().peersDroppedSilent != 1 ||
			    fx.host.GetStats().timeoutReason.find("MissingFrameTimeout") != std::string::npos) {
				*error = "the host did not adjudicate the wedged peer: " + fx.host.BuildReportJson();
				return false;
			}
			// The silence bound takes the seat, so it takes the connection with it. A wedged peer is not
			// running to hear the close; the host is the one that must stop paying for it.
			if (fx.hostT.IsPeerConnected(2) || fx.host.GetStats().connectionsClosedOnEviction != 1) {
				*error = "the wedged peer's connection outlived its seat (closed=" +
				         std::to_string(fx.host.GetStats().connectionsClosedOnEviction) + "): " + fx.host.BuildReportJson();
				return false;
			}
			// Every survivor drops the requirement at the SAME frame, or their committed sets diverge.
			if (fx.host.GetPeerLeaveFrames().at(3) != fx.clientA.GetPeerLeaveFrames().at(3)) {
				*error = "host and survivor disagreed on the leave frame";
				return false;
			}
			fx.Collect(fx.host, hostReady);
			fx.Collect(fx.clientA, aReady);
			const size_t hostAtLeave = hostReady.size();
			const size_t aAtLeave = aReady.size();
			fx.DriveProducing(false, [&] {
				fx.Collect(fx.host, hostReady);
				fx.Collect(fx.clientA, aReady);
				return false;
			}, fx.now + timeoutMs);
			if (aReady.size() != aAtLeave || hostReady.size() != hostAtLeave) {
				*error = "survivors committed frames while the wedged seat was held";
				return false;
			}
			fx.host.ResolveHeldSeat(3, NetLockstepHoldResolution::Expired, fx.now);
			if (!fx.DriveProducing(false, [&] {
					fx.Collect(fx.host, hostReady);
					fx.Collect(fx.clientA, aReady);
					return aReady.size() >= aAtLeave + 4 && hostReady.size() >= hostAtLeave + 4;
				}, fx.now + 4 * timeoutMs)) {
				*error = "the survivors did not resume after Expired (host=" + std::to_string(hostReady.size()) +
				         " a=" + std::to_string(aReady.size()) + ")";
				return false;
			}
			if (fx.host.IsFailed() || fx.clientA.IsFailed() || fx.host.GetPeerLeaveFrames().count(2) != 0) {
				*error = "a survivor was failed or dropped after the wedged peer was adjudicated: " + fx.host.BuildReportJson();
				return false;
			}
			return true;
		}

		// The negative control for the rule above: a client NEVER adjudicates. Two survivors judging
		// independently would drop the same peer at different frames and diverge, so a client whose
		// host has gone quiet must still die on its own missing-frame grace, with the same text.
		bool TestCoordinatorSilentHostStillTimesOut(std::string* error) {
			StarFixture fx;
			const uint32_t timeoutMs = 300;
			if (!fx.Start(43013, 0x7000000000000013ULL, timeoutMs, error)) {
				return false;
			}
			if (!fx.Drive(true, true, true, [&] { return fx.Running(); }, 2000)) {
				*error = "silent-host fixture did not reach Running";
				return false;
			}
			std::vector<uint64_t> aReady;
			if (!fx.DriveProducing(true, [&] {
					fx.Collect(fx.clientA, aReady);
					return aReady.size() >= 2;
				}, fx.now + timeoutMs)) {
				*error = "silent-host fixture did not get the round moving";
				return false;
			}
			// The host wedges: A now waits on the host and, through it, on B.
			if (!fx.Drive(false, true, true, [&] { return fx.clientA.IsFailed(); }, fx.now + 6 * timeoutMs)) {
				*error = "a client with a silent host did not time out";
				return false;
			}
			if (fx.clientA.GetStats().timeoutReason.find("MissingFrameTimeout") == std::string::npos) {
				*error = "a client with a silent host stopped for the wrong reason: " + fx.clientA.GetStats().timeoutReason;
				return false;
			}
			if (!fx.clientA.GetPeerLeaveFrames().empty() || fx.clientA.GetStats().peersDroppedSilent != 0) {
				*error = "a client adjudicated a peer drop, which only the relay host may do";
				return false;
			}
			return true;
		}

		// Runs a 3-peer star to a moving round, then refuses every host send to clientB while all three
		// keep producing - so the ONLY thing wrong is the forward, not a peer that went quiet.
		bool StartRelayRefusal(StarFixture& fx, uint16_t port, uint64_t sessionId, uint32_t timeoutMs, std::vector<uint64_t>& hostReady, std::string* error) {
			if (!fx.Start(port, sessionId, timeoutMs, error)) {
				return false;
			}
			if (!fx.Drive(true, true, true, [&] { return fx.Running(); }, 2000)) {
				*error = "relay-refusal fixture did not reach Running";
				return false;
			}
			if (!fx.DriveProducing(true, [&] {
					fx.Collect(fx.host, hostReady);
					return hostReady.size() >= 2;
				}, fx.now + timeoutMs)) {
				*error = "relay-refusal fixture did not get the round moving";
				return false;
			}
			LoopbackTransportConfig refuseB;
			refuseB.refuseSendsToPeer = 2; // clientB's host-side transport id.
			fx.hostT.SetFaultConfig(refuseB);
			return true;
		}

		// A refused forward was never queued and the receiver cannot ask for it again, so the host
		// holds it and retries: a send buffer that frees up costs a hitch, not a player.
		bool TestCoordinatorRelayBacklogHeals(std::string* error) {
			StarFixture fx;
			const uint32_t timeoutMs = 2000; // Refusals are bounded at half of this; heal well inside it.
			std::vector<uint64_t> hostReady, bReady;
			if (!StartRelayRefusal(fx, 43014, 0x7000000000000014ULL, timeoutMs, hostReady, error)) {
				return false;
			}
			const uint64_t refusedFrom = fx.now;
			fx.DriveProducing(true, [&] { return fx.now >= refusedFrom + 200; }, refusedFrom + 200);
			if (fx.host.GetStats().relaySendFailures == 0) {
				*error = "the refusal never reached the relay: " + fx.host.BuildReportJson();
				return false;
			}
			fx.hostT.SetFaultConfig({});
			fx.Collect(fx.clientB, bReady);
			const size_t behind = bReady.size();
			if (!fx.DriveProducing(true, [&] {
					fx.Collect(fx.clientB, bReady);
					return bReady.size() >= behind + 4;
				}, fx.now + timeoutMs)) {
				*error = "a peer whose forwards were refused only briefly did not catch up: " + fx.host.BuildReportJson();
				return false;
			}
			const NetLockstepStats& stats = fx.host.GetStats();
			if (stats.relayResends == 0 || stats.peers.at(3).relayResends == 0) {
				*error = "the retried forwards were not counted: " + fx.host.BuildReportJson();
				return false;
			}
			if (!fx.host.GetPeerLeaveFrames().empty() || fx.host.IsFailed() || fx.clientB.IsFailed()) {
				*error = "a peer was dropped for a refusal that healed: " + fx.host.BuildReportJson();
				return false;
			}
			return true;
		}

		// A forward the transport keeps refusing IS a gap the receiver can never fill; it would hang
		// on that frame until its grace ran out and take the other clients with it. Bound it.
		bool TestCoordinatorRelayFailureDropsPeer(std::string* error) {
			StarFixture fx;
			const uint32_t timeoutMs = 400; // Refusals are bounded on the same budget as silence: 200ms.
			std::vector<uint64_t> hostReady, aReady;
			if (!StartRelayRefusal(fx, 43015, 0x7000000000000015ULL, timeoutMs, hostReady, error)) {
				return false;
			}
			if (!fx.DriveProducing(true, [&] { return fx.host.GetPeerLeaveFrames().count(3) != 0; }, fx.now + 4 * timeoutMs)) {
				*error = "the host kept a peer it could not reach: " + fx.host.BuildReportJson();
				return false;
			}
			const NetLockstepStats& stats = fx.host.GetStats();
			if (stats.relaySendFailures == 0 || stats.peers.at(3).relaySendFailures == 0 || stats.lastRelayError.empty()) {
				*error = "the refused relay was not counted: " + fx.host.BuildReportJson();
				return false;
			}
			// It has to be the unreachable bound: clientB's own frames keep reaching the host, so it
			// is never the silent one, and the two paths must stay distinguishable in the report.
			if (stats.peersDroppedSilent != 0 || stats.relayPacketsSent == 0 || stats.peers.at(2).relayPacketsSent == 0) {
				*error = "the peer was dropped for the wrong reason: " + fx.host.BuildReportJson();
				return false;
			}
			fx.Collect(fx.host, hostReady);
			fx.Collect(fx.clientA, aReady);
			const size_t committedAtLeave = aReady.size();
			fx.DriveProducing(true, [&] {
				fx.Collect(fx.host, hostReady);
				fx.Collect(fx.clientA, aReady);
				return false;
			}, fx.now + timeoutMs);
			if (aReady.size() != committedAtLeave) {
				*error = "survivors committed frames while the unreachable seat was held: " + fx.host.BuildReportJson();
				return false;
			}
			fx.host.ResolveHeldSeat(3, NetLockstepHoldResolution::Expired, fx.now);
			if (!fx.DriveProducing(true, [&] {
					fx.Collect(fx.host, hostReady);
					fx.Collect(fx.clientA, aReady);
					return aReady.size() >= committedAtLeave + 4;
				}, fx.now + 4 * timeoutMs)) {
				*error = "the survivors did not resume after Expired: " + fx.host.BuildReportJson();
				return false;
			}
			if (fx.host.IsFailed() || fx.clientA.IsFailed() || fx.clientA.GetPeerLeaveFrames().count(3) == 0) {
				*error = "the survivor did not follow the host past the unreachable peer";
				return false;
			}
			const std::string report = fx.host.BuildReportJson();
			if (report.find("\"relay_send_failures\":") == std::string::npos ||
			    report.find("\"peers\":{") == std::string::npos ||
			    report.find("\"frames_contributed\":") == std::string::npos) {
				*error = "the per-peer relay counters are missing from the report: " + report;
				return false;
			}
			return true;
		}

		// The H4 seat state the round asks for, stubbed. This case pins the coordinator half of the
		// hold; the plane's own answer is pinned by -net-reconnect-session-selftest.
		struct SeatStateStub {
			bool held = false;
			uint8_t heldPeerId = 0; //!< 0 holds every peer's seat; otherwise only this one's.
			NetPeerId fenced = c_InvalidNetPeerId;
		};

		NetLockstepSeatState QuerySeatStateStub(void* context, uint8_t peerId, NetPeerId transportPeerId) {
			auto* stub = static_cast<SeatStateStub*>(context);
			NetLockstepSeatState state;
			state.heldForReclaim = stub->held && (stub->heldPeerId == 0 || stub->heldPeerId == peerId);
			state.fencedTransport = transportPeerId != c_InvalidNetPeerId && transportPeerId == stub->fenced;
			return state;
		}

		// The surviving client answers seat state the way NetMatchService::QuerySeatState does off the
		// host (NetMatchService.cpp:1125): a default state for every seat. So a hold that is read from
		// the admission plane exists only on the host, and one derived from the leave notice exists on
		// every peer - which is the whole point of the frame deadline.
		struct ClientSeatStateProbe {
			uint32_t reads = 0;
		};

		NetLockstepSeatState QuerySeatStateAsAClient(void* context, uint8_t, NetPeerId) {
			++static_cast<ClientSeatStateProbe*>(context)->reads;
			return NetLockstepSeatState{};
		}

		// One of two remotes drops and the other plays on - the shape every substitution gate sets up.
		// The scripted outcome is declared inside the sim on every peer, so both survivors must answer
		// the activity gate identically at the same frame, through the hold, at its deadline and past it.
		bool TestActivityGateAgreesAcrossPeers(std::string* error) {
			const uint16_t port = 43072;
			const uint64_t sessionId = 0x7000000000000072ULL;
			LoopbackTransport hostT, leaverT, stayerT;
			if (!hostT.StartHost(port, error) || !leaverT.Connect("loopback", port, error) || !stayerT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.timeoutMs = 5000;
				c.localPeerId = local;
				c.peerCount = 3;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			SeatStateStub hostStub;
			hostStub.held = true;
			ClientSeatStateProbe stayerProbe;
			NetLockstepCoordinator host, leaver, stayer;
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error) ||
			    !leaver.Start(leaverT, cfg(2, {{1, 1}}, false), error) ||
			    !stayer.Start(stayerT, cfg(3, {{1, 1}}, false), error)) {
				return false;
			}
			host.SetSeatStateSource(&QuerySeatStateStub, &hostStub);
			stayer.SetSeatStateSource(&QuerySeatStateAsAClient, &stayerProbe);
			uint64_t now = 0;
			auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
				for (const uint64_t until = now + forMs; now <= until; now += 5) {
					host.Tick(now);
					leaver.Tick(now);
					stayer.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					leaverT.AdvanceTimeMs(5);
					stayerT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive(2000, [&] { return host.IsRunning() && leaver.IsRunning() && stayer.IsRunning(); })) {
				*error = "the three-peer round never started";
				return false;
			}
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !leaver.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error) ||
				    !stayer.QueueLocalInput(f, {MakeFrame(300, f + 1)}, {}, error)) {
					return false;
				}
			}
			NetLockstepReadyFrame ready;
			size_t committed = 0;
			if (!drive(2000, [&] {
					while (host.PopReadyFrame(ready)) {
						++committed;
					}
					while (stayer.PopReadyFrame(ready)) {
					}
					return committed >= 2;
				})) {
				*error = "the three-peer round never committed a frame";
				return false;
			}

			leaverT.Stop();
			if (!drive(3000, [&] { return host.GetPeerLeaveFrames().count(2) != 0 && stayer.GetPeerLeaveFrames().count(2) != 0; })) {
				*error = "both survivors never adjudicated the drop";
				return false;
			}
			if (!host.IsRunning() || !stayer.IsRunning()) {
				*error = "a survivor left the round";
				return false;
			}
			const uint64_t hostLeaveFrame = host.GetPeerLeaveFrames().at(2);
			if (stayer.GetPeerLeaveFrames().at(2) != hostLeaveFrame) {
				*error = "the two survivors recorded different leave frames";
				return false;
			}

			// The production call, asked of each peer still playing, at the frame it is applying.
			auto gateAt = [](NetLockstepCoordinator& peer, uint64_t frame) {
				ScenarioRunner::SetLockstepCoordinator(&peer);
				ScenarioRunner::SetLockstepAppliedFrame(frame);
				const bool gate = ScenarioRunner::IsLockstepHoldingSeatForReclaim();
				ScenarioRunner::SetLockstepCoordinator(nullptr);
				return gate;
			};
			const uint64_t deadline = hostLeaveFrame + NetLockstepCoordinator::c_ReclaimHoldFrames;
			const std::vector<std::pair<const char*, uint64_t>> samples = {
			    {"at_the_drop", hostLeaveFrame},
			    {"mid_window", hostLeaveFrame + NetLockstepCoordinator::c_ReclaimHoldFrames / 2},
			    {"last_held_frame", deadline - 1},
			    {"at_the_deadline", deadline},
			    {"past_the_deadline", deadline + 600},
			};
			bool agreed = true;
			std::string disagreement;
			for (const auto& [name, frame]: samples) {
				const bool hostGate = gateAt(host, frame);
				const bool stayerGate = gateAt(stayer, frame);
				const bool hostDefers = ActivityMan::ScriptedEndIsDeferred(true, true, hostGate);
				const bool stayerDefers = ActivityMan::ScriptedEndIsDeferred(true, true, stayerGate);
				std::cout << "[net-lockstep-selftest] MEASURE activity_gate_across_peers " << name
				          << " frame=" << frame << " host_defers=" << hostDefers << " stayer_defers=" << stayerDefers
				          << " stayer_seat_reads=" << stayerProbe.reads << std::endl;
				if (hostDefers != stayerDefers) {
					agreed = false;
					disagreement = name;
					break;
				}
				if (!hostDefers) {
					*error = std::string("the hold released before a host resolution at ") + name;
					return false;
				}
			}
			if (!host.AnyDroppedSeatHeld() || !stayer.AnyDroppedSeatHeld()) {
				*error = "a survivor released the hold before a host resolution";
				return false;
			}
			if (!agreed) {
				*error = "the activity gate disagrees across peers still in the round: the host defers a "
				         "scripted outcome the surviving client applies (" + disagreement + ")";
				return false;
			}
			// Ownership's own question is untouched: a survivor is still here, so it stays false on both.
			if (host.IsHoldingSeatForReclaim() || stayer.IsHoldingSeatForReclaim()) {
				*error = "a round with a surviving remote reported itself as holding for reclaim";
				return false;
			}
			host.ResolveHeldSeat(2, NetLockstepHoldResolution::Expired, now);
			if (!drive(2000, [&] { return !host.AnyDroppedSeatHeld() && !stayer.AnyDroppedSeatHeld(); })) {
				*error = "the expired resolution never reached both survivors";
				return false;
			}
			if (gateAt(host, deadline + 600) || gateAt(stayer, deadline + 600)) {
				*error = "the activity gate stayed deferred after Expired";
				return false;
			}
			return true;
		}

		bool B2SnapshotFailure(std::string* error, const std::string& message) {
			*error = "seat snapshot: " + message;
			return false;
		}

		std::vector<NetSeatPresenceEntry> B2SnapshotSeats(uint8_t count) {
			std::vector<NetSeatPresenceEntry> seats;
			for (uint8_t i = 0; i < count; ++i) {
				NetSeatPresenceEntry seat;
				seat.stableSeat = i;
				seat.peerId = i + 1;
				seat.holderGeneration = 10 + i;
				seat.seatGeneration = 20 + i;
				seat.incarnation = 30 + i;
				seat.holderName = i == 0 ? "Host" : "Holder " + std::to_string(i + 1);
				seats.push_back(std::move(seat));
			}
			return seats;
		}

		NetLockstepSeatSnapshot B2SnapshotPacket(uint8_t count) {
			NetLockstepSeatSnapshot snapshot;
			snapshot.senderPeerId = 1;
			snapshot.sessionId = 0x0102030405060708ULL;
			for (size_t i = 0; i < snapshot.epoch.size(); ++i) snapshot.epoch[i] = static_cast<uint8_t>(i);
			snapshot.roundId = 0x1112131415161718ULL;
			snapshot.revision = 0x2122232425262728ULL;
			snapshot.observedAtMs = 0x3132333435363738ULL;
			snapshot.seats = B2SnapshotSeats(count);
			return snapshot;
		}

		void B2SnapshotWriteLE(std::vector<uint8_t>& bytes, size_t offset, uint64_t value, size_t width) {
			for (size_t i = 0; i < width; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
		}

		void B2SnapshotFixLength(std::vector<uint8_t>& bytes) {
			B2SnapshotWriteLE(bytes, 12, bytes.size() - NetLockstepCodec::c_HeaderBytes, 4);
		}

		bool B2SnapshotRefused(const std::vector<uint8_t>& bytes, const std::string& context, std::string* error) {
			if (NetLockstepCodec::Decode(bytes).ok) return B2SnapshotFailure(error, "decoded " + context);
			return true;
		}

		bool TestB2SeatSnapshotCodec(std::string* error) {
			NetLockstepSeatSnapshot canonical = B2SnapshotPacket(1);
			auto& seat = canonical.seats.front();
			seat.state = NetSeatPresenceState::Disconnected;
			seat.holderGeneration = 0x11223344;
			seat.seatGeneration = 0x55667788;
			seat.incarnation = 0x99AABBCC;
			seat.holdActive = true;
			seat.holdUntilMs = 0x4142434445464748ULL;
			seat.holdUntilFrame = 0x5152535455565758ULL;
			seat.holderName = "A";
			const std::vector<uint8_t> expected = {
				0x43, 0x43, 0x4C, 0x33, 0x14, 0x00, 0x10, 0x00, 0x06, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00,
				0x01, 0x01, 0x00, 0x00, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
				0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
				0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
				0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
				0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31,
				0x00, 0x00, 0x01, 0x01, 0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55, 0xCC, 0xBB, 0xAA, 0x99,
				0x01, 0x48, 0x47, 0x46, 0x45, 0x44, 0x43, 0x42, 0x41,
				0x58, 0x57, 0x56, 0x55, 0x54, 0x53, 0x52, 0x51, 0x01, 0x00, 0x41,
			};
			std::vector<uint8_t> bytes;
			if (!EncodePacket({canonical}, bytes, error)) return false;
			if (bytes != expected || !NetLockstepCodec::LooksLikePacket(bytes) ||
			    NetLockstepCodec::PacketTypeOf(NetLockstepPayload{canonical}) != NetLockstepPacketType::SeatSnapshot ||
			    std::string(NetLockstepCodec::PacketTypeName(NetLockstepPacketType::SeatSnapshot)) != "SeatSnapshot") {
				return B2SnapshotFailure(error, "canonical v17 bytes or packet type differ");
			}
			const auto decoded = NetLockstepCodec::Decode(expected);
			if (!decoded.ok || decoded.packet != NetLockstepPacket{canonical}) return B2SnapshotFailure(error, "canonical fixture did not decode exactly");
			std::vector<uint8_t> reencoded;
			if (!EncodePacket(decoded.packet, reencoded, error) || reencoded != expected) return B2SnapshotFailure(error, "decode/encode was not canonical");

			const auto base = B2SnapshotPacket(3);
			if (!EncodePacket({base}, bytes, error)) return false;
			for (size_t length = 0; length < bytes.size(); ++length) {
				std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + length);
				const auto outer = NetLockstepCodec::Decode(cut);
				const auto expectedError = length < NetLockstepCodec::c_HeaderBytes ? NetLockstepErrorCode::ShortHeader : NetLockstepErrorCode::PayloadLengthMismatch;
				if (outer.ok || outer.error.code != expectedError) return B2SnapshotFailure(error, "outer truncation accepted or misclassified at " + std::to_string(length));
				if (length >= NetLockstepCodec::c_HeaderBytes) {
					B2SnapshotFixLength(cut);
					const auto inner = NetLockstepCodec::Decode(cut);
					if (inner.ok || inner.error.code != NetLockstepErrorCode::TruncatedPayload) return B2SnapshotFailure(error, "payload truncation accepted or misclassified at " + std::to_string(length));
				}
			}

			const size_t first = NetLockstepCodec::c_HeaderBytes + 52;
			const size_t second = first + 35 + base.seats[0].holderName.size();
			const size_t third = second + 35 + base.seats[1].holderName.size();
			const std::vector<std::pair<const char*, std::function<void(std::vector<uint8_t>&)>>> malformed = {
				{"zero sender", [](auto& b) { b[16] = 0; }},
				{"out-of-range sender", [](auto& b) { b[16] = NetLockstepCodec::c_MaxPeerCount + 1; }},
				{"too many subjects", [](auto& b) { b[17] = NetLockstepCodec::c_MaxPeerCount + 1; }},
				{"too few declared subjects", [](auto& b) { b[17] = 2; }},
				{"empty full snapshot", [](auto& b) { b[17] = 0; b.resize(68); B2SnapshotFixLength(b); }},
				{"snapshot reserved low byte", [](auto& b) { b[18] = 1; }},
				{"snapshot reserved high byte", [](auto& b) { b[19] = 1; }},
				{"outer flag", [](auto& b) { b[10] = 1; }},
				{"zero session", [](auto& b) { B2SnapshotWriteLE(b, 20, 0, 8); }},
				{"zero epoch", [](auto& b) { std::fill(b.begin() + 28, b.begin() + 44, 0); }},
				{"zero round", [](auto& b) { B2SnapshotWriteLE(b, 44, 0, 8); }},
				{"zero revision", [](auto& b) { B2SnapshotWriteLE(b, 52, 0, 8); }},
				{"duplicate stable seat", [=](auto& b) { B2SnapshotWriteLE(b, second, 0, 2); }},
				{"descending stable seats", [=](auto& b) { B2SnapshotWriteLE(b, second, 4, 2); B2SnapshotWriteLE(b, third, 1, 2); }},
				{"duplicate peer", [=](auto& b) { b[second + 2] = b[first + 2]; }},
				{"zero subject peer", [=](auto& b) { b[first + 2] = 0; }},
				{"out-of-range subject peer", [=](auto& b) { b[first + 2] = NetLockstepCodec::c_MaxPeerCount + 1; }},
				{"unknown state", [=](auto& b) { b[first + 3] = 5; }},
				{"noncanonical hold flag", [=](auto& b) { b[first + 16] = 2; }},
				{"inactive hold with a deadline", [=](auto& b) { B2SnapshotWriteLE(b, first + 17, 1000, 8); }},
				{"present seat with an active hold", [=](auto& b) { b[first + 16] = 1; B2SnapshotWriteLE(b, first + 17, 1000, 8); }},
				{"oversized name", [=](auto& b) { B2SnapshotWriteLE(b, first + 33, NetProtocol::c_MaxDisplayNameBytes + 1, 2); }},
				{"name control character", [=](auto& b) { b[first + 35] = '\n'; }},
				{"name embedded NUL", [=](auto& b) { b[first + 35] = 0; }},
				{"trailing payload", [](auto& b) { b.push_back(0); B2SnapshotFixLength(b); }},
			};
			for (const auto& [name, mutate]: malformed) {
				auto bad = bytes;
				mutate(bad);
				if (!B2SnapshotRefused(bad, name, error)) return false;
			}
			for (uint16_t version = NetLockstepCodec::c_MinVersion; version < 17; ++version) {
				auto old = bytes;
				B2SnapshotWriteLE(old, 4, version, 2);
				const auto result = NetLockstepCodec::Decode(old);
				if (result.ok || result.error.code != NetLockstepErrorCode::UnsupportedVersion) return B2SnapshotFailure(error, "seat packet accepted under old version " + std::to_string(version));
			}
			for (uint16_t type = 1; type < 6; ++type) {
				auto retagged = bytes;
				B2SnapshotWriteLE(retagged, 8, type, 2);
				if (!B2SnapshotRefused(retagged, "seat payload tagged as type " + std::to_string(type), error)) return false;
			}
			NetLockstepStart start;
			start.sessionId = 5; start.localPeerId = 1; start.peerCount = 3;
			start.controllerFrameVersion = ControllerFrame::c_Version;
			start.controllerFrameEncodedSize = static_cast<uint16_t>(ControllerFrame::c_EncodedSize);
			start.scenario = "LockstepSelfTest"; start.ownershipPolicy = "unique-id-split"; start.roundId = 7;
			NetLockstepFrame frame;
			frame.senderPeerId = 1; frame.roundId = 7;
			const std::vector<NetLockstepPacket> otherTypes = {
				{start}, {frame}, {NetLockstepAck{1, 2, 3}},
				{NetLockstepStop{1, NetLockstepStopReason::Complete, 2, "done"}}, {NetLockstepChecksum{1, 2, {}, 7}},
			};
			for (const auto& packet: otherTypes) {
				std::vector<uint8_t> retagged;
				if (!EncodePacket(packet, retagged, error)) return false;
				B2SnapshotWriteLE(retagged, 8, 6, 2);
				if (!B2SnapshotRefused(retagged, "other payload tagged as SeatSnapshot", error)) return false;
			}
			for (uint16_t reason: {13, 14, 15}) {
				NetLockstepError failure;
				if (NetLockstepCodec::Encode({NetLockstepStop{1, static_cast<NetLockstepStopReason>(reason), 0, "status"}}, reencoded, &failure)) {
					return B2SnapshotFailure(error, "unknown stop reason still encodes as Stop");
				}
			}
			auto limit = B2SnapshotPacket(NetLockstepCodec::c_MaxPeerCount);
			for (auto& entry: limit.seats) entry.holderName.assign(NetProtocol::c_MaxDisplayNameBytes, 'x');
			if (!RoundTrip({limit}, error)) return false;
			limit.seats[1].holderName.push_back('x');
			NetLockstepError failure;
			if (NetLockstepCodec::Encode({limit}, reencoded, &failure) || failure.code != NetLockstepErrorCode::StringTooLong) return B2SnapshotFailure(error, "overlong name encodes");
			for (const auto invalid: {std::string("bad\nname"), std::string("bad\rname"), std::string("bad\0name", 8), std::string("bad\x7Fname")}) {
				limit = B2SnapshotPacket(3); limit.seats[1].holderName = invalid;
				if (NetLockstepCodec::Encode({limit}, reencoded, &failure) || failure.code != NetLockstepErrorCode::InvalidString) return B2SnapshotFailure(error, "invalid holder name encodes");
			}
			limit = B2SnapshotPacket(0);
			if (NetLockstepCodec::Encode({limit}, reencoded, &failure)) return B2SnapshotFailure(error, "empty complete roster encodes");
			limit = B2SnapshotPacket(NetLockstepCodec::c_MaxPeerCount + 1);
			if (NetLockstepCodec::Encode({limit}, reencoded, &failure)) return B2SnapshotFailure(error, "too many subjects encode");
			const std::vector<std::pair<const char*, std::function<void(NetLockstepSeatSnapshot&)>>> invalidObjects = {
				{"zero sender", [](auto& s) { s.senderPeerId = 0; }},
				{"zero session", [](auto& s) { s.sessionId = 0; }},
				{"zero epoch", [](auto& s) { s.epoch.fill(0); }},
				{"zero round", [](auto& s) { s.roundId = 0; }},
				{"zero revision", [](auto& s) { s.revision = 0; }},
				{"duplicate seat", [](auto& s) { s.seats[1].stableSeat = s.seats[0].stableSeat; }},
				{"descending seats", [](auto& s) { std::swap(s.seats[0], s.seats[1]); }},
				{"duplicate peer", [](auto& s) { s.seats[1].peerId = s.seats[0].peerId; }},
				{"zero subject", [](auto& s) { s.seats[1].peerId = 0; }},
				{"unknown state", [](auto& s) { s.seats[1].state = static_cast<NetSeatPresenceState>(5); }},
				{"inactive hold deadline", [](auto& s) { s.seats[1].holdUntilMs = 1; }},
				{"active hold on a present seat", [](auto& s) { s.seats[1].holdActive = true; s.seats[1].holdUntilMs = 1; }},
			};
			for (const auto& [name, mutate]: invalidObjects) {
				limit = B2SnapshotPacket(3); mutate(limit);
				if (NetLockstepCodec::Encode({limit}, reencoded, &failure)) return B2SnapshotFailure(error, std::string("encoded invalid object: ") + name);
			}
			return true;
		}

		class B2SnapshotTransport : public LoopbackTransport {
		public:
			struct Sent {
				NetPeerId peer;
				NetTransportLane lane;
				NetLockstepSeatSnapshot snapshot;
				bool accepted;
				bool congested;
			};
			std::vector<Sent> sent;
			std::vector<NetLockstepSeatSnapshot> received;
			std::vector<NetTransportEvent> injected;

			bool Send(NetPeerId peer, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error = nullptr, bool* congested = nullptr) override {
				bool refusedForCongestion = false;
				const bool accepted = LoopbackTransport::Send(peer, lane, bytes, error, &refusedForCongestion);
				if (congested) *congested = refusedForCongestion;
				const auto decoded = NetLockstepCodec::Decode(bytes);
				if (decoded.ok) {
					if (const auto* snapshot = std::get_if<NetLockstepSeatSnapshot>(&decoded.packet.payload)) sent.push_back({peer, lane, *snapshot, accepted, refusedForCongestion});
				}
				return accepted;
			}

			std::vector<NetTransportEvent> PollEvents() override {
				auto events = LoopbackTransport::PollEvents();
				for (auto& event: injected) events.push_back(std::move(event));
				injected.clear();
				for (const auto& event: events) {
					if (event.type != NetTransportEventType::PacketReceived) continue;
					const auto decoded = NetLockstepCodec::Decode(event.bytes);
					if (decoded.ok) {
						if (const auto* snapshot = std::get_if<NetLockstepSeatSnapshot>(&decoded.packet.payload)) received.push_back(*snapshot);
					}
				}
				return events;
			}
		};

		struct B2SnapshotRound {
			B2SnapshotTransport transport[4];
			NetLockstepCoordinator owned[4];
			NetLockstepCoordinator* peers[4] = {&owned[0], &owned[1], &owned[2], &owned[3]};
			NetSeatPresence views[4];
			NetLockstepConfig config[4];
			std::optional<NetLockstepSeatSnapshot> latest[4];
			std::array<size_t, 4> reads{};
			bool active[4] = {true, true, true, true};
			NetPeerId hostSideId[4] = {0, 1, 2, 3};
			uint8_t count = 0;
			uint16_t port = 0;
			uint64_t now = 0;
			bool viewRejected = false;

			void Pump(unsigned steps = 4) {
				for (unsigned step = 0; step < steps; ++step) {
					now += 5;
					for (uint8_t i = 0; i < count; ++i) transport[i].AdvanceTimeMs(5);
					for (uint8_t i = 0; i < count; ++i) {
						if (!active[i]) continue;
						peers[i]->Tick(now);
						if (const auto snapshot = peers[i]->TakeSeatSnapshot()) {
							latest[i] = *snapshot;
							++reads[i];
							if (!views[i].ApplySnapshot(*snapshot, now)) viewRejected = true;
						}
					}
				}
			}

			bool Running(std::string* error) const {
				for (uint8_t i = 0; i < count; ++i) {
					if (active[i] && !peers[i]->IsRunning()) return B2SnapshotFailure(error, "peer " + std::to_string(i + 1) + " left Running: " + peers[i]->GetStats().timeoutReason);
				}
				return !viewRejected || B2SnapshotFailure(error, "presentation rejected a coordinator snapshot");
			}

			bool StartRound(uint64_t roundId, uint64_t startFrame, std::string* error) {
				for (uint8_t i = 0; i < count; ++i) {
					config[i].roundId = i == 0 ? roundId : 0;
					config[i].startFrame = startFrame;
					config[i].remoteTransportPeerIds.clear();
					if (i == 0) {
						for (uint8_t j = 1; j < count; ++j) config[i].remoteTransportPeerIds.emplace(j + 1, hostSideId[j]);
					} else {
						config[i].remoteTransportPeerIds.emplace(1, 1);
					}
					latest[i].reset(); reads[i] = 0;
					if (!peers[i]->Start(transport[i], config[i], error)) return false;
				}
				Pump(10);
				return Running(error);
			}

			bool Start(uint8_t peerCount, uint16_t listenPort, std::string* error) {
				count = peerCount; port = listenPort;
				if (!transport[0].StartHost(port, error)) return false;
				for (uint8_t i = 1; i < count; ++i) if (!transport[i].Connect("loopback", port, error)) return false;
				for (uint8_t i = 0; i < count; ++i) {
					auto& c = config[i];
					c.sessionId = 0xB200000000000000ULL + port;
					c.timeoutMs = 60000; c.localPeerId = i + 1; c.peerCount = count;
					c.relayToOtherPeers = i == 0;
					c.scenario = "LockstepSelfTest"; c.ownershipPolicy = "unique-id-split";
					c.matchConfig.sessionId = c.sessionId; c.matchConfig.peerCount = count;
					c.matchConfig.ownershipPolicy = NetActorOwnershipPolicy::UniqueIdModPeerCount;
					for (uint8_t j = 0; j < count; ++j) c.matchConfig.players.push_back({static_cast<uint8_t>(j + 1), j, false, "Original " + std::to_string(j + 1)});
					for (size_t j = 0; j < c.seatPresenceEpoch.size(); ++j) c.seatPresenceEpoch[j] = static_cast<uint8_t>(j + 1);
				}
				return StartRound(0xB217000000000000ULL + port, 0, error);
			}

			bool Equals(const NetLockstepSeatSnapshot& expected, std::string* error) const {
				if (!Running(error)) return false;
				for (uint8_t i = 0; i < count; ++i) {
					if (!active[i]) continue;
					if (!latest[i] || *latest[i] != expected || views[i].GetSnapshot() != std::optional<NetLockstepSeatSnapshot>{expected}) {
						return B2SnapshotFailure(error, "full snapshot differs on peer " + std::to_string(i + 1) + " at revision " + std::to_string(expected.revision));
					}
					if (views[i].GetSeats().size() != expected.seats.size()) return B2SnapshotFailure(error, "presentation dropped a subject");
					for (const auto& entry: expected.seats) {
						const auto found = views[i].GetSeats().find(entry.peerId);
						if (found == views[i].GetSeats().end() || found->second != entry) return B2SnapshotFailure(error, "presentation metadata differs");
					}
				}
				return true;
			}

			bool Publish(const std::vector<NetSeatPresenceEntry>& seats, uint64_t observedAt, std::string* error) {
				NetLockstepSeatSnapshot expected;
				expected.senderPeerId = 1; expected.sessionId = config[0].sessionId; expected.epoch = config[0].seatPresenceEpoch;
				expected.roundId = peers[0]->GetRoundId(); expected.revision = latest[0] ? latest[0]->revision + 1 : 1;
				expected.observedAtMs = observedAt; expected.seats = seats;
				if (!peers[0]->PublishSeatSnapshot(seats, observedAt)) return B2SnapshotFailure(error, "host refused a legitimate update");
				Pump();
				return Equals(expected, error);
			}

			bool SendToClients(const NetLockstepPacket& packet, std::string* error) {
				std::vector<uint8_t> bytes;
				if (!EncodePacket(packet, bytes, error)) return false;
				for (uint8_t i = 1; i < count; ++i) {
					if (active[i] && !transport[0].Send(hostSideId[i], NetTransportLane::ControlReliable, bytes, error)) return false;
				}
				Pump();
				return true;
			}

			std::vector<uint64_t> Authority() const {
				std::vector<uint64_t> result;
				for (uint8_t i = 0; i < count; ++i) {
					if (!active[i]) continue;
					const auto& peer = *peers[i];
					result.push_back(peer.GetStats().framesAccepted); result.push_back(peer.GetStats().nextFrame);
					result.push_back(peer.GetStats().remoteControllerFramesReceived); result.push_back(peer.GetRoundId());
					for (uint8_t subject = 1; subject <= count; ++subject) {
						result.push_back(peer.IsPeerGoneAtFrame(subject, UINT64_MAX));
						result.push_back(peer.ResolveActorOwner(subject, subject - 1, false));
						result.push_back(peer.ResolveActorOwner(subject, subject - 1, true));
						result.push_back(peer.ResolveTeamCommandAuthority(subject - 1));
					}
					for (const auto& [subject, frame]: peer.GetPeerLeaveFrames()) { result.push_back(subject); result.push_back(frame); }
				}
				return result;
			}

			bool Commit(uint64_t frame, std::string* error) {
				size_t live = 0;
				for (uint8_t i = 0; i < count; ++i) {
					if (!active[i]) continue;
					++live;
					if (!peers[i]->QueueLocalInput(frame, {MakeFrame(100 + i * 100, frame + 1)}, {}, error)) return false;
				}
				Pump(10);
				if (!Running(error)) return false;
				for (uint8_t i = 0; i < count; ++i) {
					if (!active[i]) continue;
					NetLockstepReadyFrame ready;
					if (!peers[i]->PopReadyFrame(ready) || ready.frame != frame || ready.localFrames.size() != 1 || ready.remoteFrames.size() != live - 1) {
						return B2SnapshotFailure(error, "control frame did not commit all required senders on peer " + std::to_string(i + 1));
					}
					if (peers[i]->PopReadyFrame(ready)) return B2SnapshotFailure(error, "unexpected extra committed frame");
				}
				return true;
			}
		};

		bool TestB2SeatSnapshotRelay(std::string* error) {
			for (uint8_t count: {3, 4}) {
				B2SnapshotRound round;
				if (!round.Start(count, 44800 + count, error) || !round.Commit(0, error)) return false;
				auto seats = B2SnapshotSeats(count);
				const auto authority = round.Authority();
				uint64_t observed = 100000;
				if (!round.Publish(seats, observed++, error)) return false;
				auto& subject = seats[1];
				subject.state = NetSeatPresenceState::Disconnected; subject.holdActive = true;
				subject.holdUntilMs = observed + 20000; subject.holdUntilFrame = 1201;
				if (!round.Publish(seats, observed++, error)) return false;
				NetSeatPresence countdown;
				if (!countdown.ApplySnapshot(*round.latest[0], 900)) return B2SnapshotFailure(error, "fresh view refused the drop");
				countdown.NoteFrame(1);
				if (countdown.HoldFramesRemaining(2) != 1200 || countdown.HoldWallSecondsRemaining(2, 900) != 20 ||
				    countdown.HoldWallSecondsRemaining(2, 901) != 20 || countdown.HoldWallSecondsRemaining(2, 20899) != 1 ||
				    countdown.HoldWallSecondsRemaining(2, 20900) != 0) return B2SnapshotFailure(error, "hold countdown is not relative to receipt");
				countdown.NoteFrame(50000);
				if (countdown.HoldFramesRemaining(2) != 0 || countdown.StateOf(2) != NetSeatPresenceState::Disconnected ||
				    countdown.HoldWallSecondsRemaining(2, UINT64_MAX) != 0) return B2SnapshotFailure(error, "hold expiry revoked return eligibility");
				subject.state = NetSeatPresenceState::Reconnecting;
				if (!round.Publish(seats, observed++, error)) return false;
				subject.state = NetSeatPresenceState::Disconnected;
				if (!round.Publish(seats, observed++, error)) return false;
				subject.holdActive = false; subject.holdUntilMs = 0;
				if (!round.Publish(seats, observed++, error)) return false;
				subject.state = NetSeatPresenceState::Reconnecting;
				if (!round.Publish(seats, observed++, error)) return false;
				subject.state = NetSeatPresenceState::Present; ++subject.incarnation; ++subject.seatGeneration; subject.holdUntilFrame = 0;
				if (!round.Publish(seats, observed++, error)) return false;
				subject.state = NetSeatPresenceState::Substituted;
				++subject.holderGeneration; ++subject.seatGeneration; subject.incarnation = 0; subject.holderName = "Understudy";
				if (!round.Publish(seats, observed++, error)) return false;
				for (uint8_t i = 0; i < count; ++i) {
					const std::string text = round.views[i].Line(2, "Former holder");
					if (text.find("Understudy") == std::string::npos || text.find("substitute") == std::string::npos || text.find("Former holder") != std::string::npos) return B2SnapshotFailure(error, "substitute line uses the former holder");
				}
				subject.state = NetSeatPresenceState::Left; ++subject.seatGeneration;
				if (!round.Publish(seats, observed++, error)) return false;
				if (round.Authority() != authority || !round.peers[0]->GetPeerLeaveFrames().empty()) return B2SnapshotFailure(error, "roster status changed simulation authority");
				for (uint8_t i = 0; i < count; ++i) {
					if (round.views[i].StateOf(1) != NetSeatPresenceState::Present || round.views[i].Line(2, "wrong").find("Understudy: left") == std::string::npos) return B2SnapshotFailure(error, "host sender was confused with its subject");
				}
				for (const auto& send: round.transport[0].sent) {
					if (!send.accepted || send.lane != NetTransportLane::ControlReliable || send.snapshot.senderPeerId != 1 || send.snapshot.seats.size() != count) return B2SnapshotFailure(error, "host did not send complete reliable snapshots");
				}
				const auto reads = round.reads;
				const size_t sent = round.transport[0].sent.size();
				if (!round.peers[0]->PublishSeatSnapshot(seats, observed + 1000)) return B2SnapshotFailure(error, "unchanged publication failed");
				round.Pump();
				if (round.reads != reads || round.transport[0].sent.size() != sent) return B2SnapshotFailure(error, "unchanged roster generated a new revision");
				if (!round.Commit(1, error)) return false;
			}
			return true;
		}

		bool TestB2SeatSnapshotAuthority(std::string* error) {
			for (uint8_t count: {3, 4}) {
				B2SnapshotRound round;
				if (!round.Start(count, 44810 + count, error) || !round.Commit(0, error) || !round.Publish(B2SnapshotSeats(count), 1000, error)) return false;
				const auto saved = *round.latest[0];
				const auto authority = round.Authority();
				const auto reads = round.reads;
				const uint64_t victimHeard = round.peers[0]->GetStats().peers.at(3).lastHeardMs;
				if (round.peers[1]->PublishSeatSnapshot(saved.seats, 2000)) return B2SnapshotFailure(error, "client published a roster");
				LoopbackTransport stranger;
				if (!stranger.Connect("loopback", round.port, error)) return false;
				for (LoopbackTransport* source: {static_cast<LoopbackTransport*>(&round.transport[1]), &stranger}) {
					for (uint8_t sender: {1, 2, 3}) {
						auto forged = saved; forged.senderPeerId = sender; ++forged.revision; ++forged.observedAtMs;
						forged.seats[1].state = NetSeatPresenceState::Left;
						std::vector<uint8_t> bytes;
						if (!EncodePacket({forged}, bytes, error)) return false;
						const size_t sent = round.transport[0].sent.size();
						if (!source->Send(1, NetTransportLane::ControlReliable, bytes, error)) return false;
						round.Pump();
						if (round.transport[0].sent.size() != sent || round.reads != reads || round.peers[0]->GetStats().peers.at(3).lastHeardMs != victimHeard || !round.Equals(saved, error)) return B2SnapshotFailure(error, "bound or unbound non-host authored a roster or refreshed a victim's liveness");
					}
				}
				auto forged = saved; ++forged.revision; ++forged.observedAtMs; forged.seats[1].state = NetSeatPresenceState::Left;
				std::vector<uint8_t> bytes;
				if (!EncodePacket({forged}, bytes, error)) return false;
				for (uint8_t i = 1; i < count; ++i) round.transport[i].injected.push_back({NetTransportEventType::PacketReceived, 999, NetTransportLane::ControlReliable, bytes, {}});
				round.Pump();
				if (round.reads != reads || !round.Equals(saved, error)) return B2SnapshotFailure(error, "client trusted an unbound transport claiming the host");
				auto truncated = bytes; truncated.pop_back(); B2SnapshotFixLength(truncated);
				if (!stranger.Send(1, NetTransportLane::ControlReliable, truncated, error)) return false;
				for (uint8_t i = 1; i < count; ++i) round.transport[i].injected.push_back({NetTransportEventType::PacketReceived, 999, NetTransportLane::ControlReliable, truncated, {}});
				round.Pump();
				if (round.reads != reads || !round.Equals(saved, error)) return B2SnapshotFailure(error, "malformed snapshot from an unbound transport ended or changed a round");
				forged.senderPeerId = 2;
				if (!round.SendToClients({forged}, error)) return false;
				if (round.reads != reads || round.Authority() != authority || !round.Equals(saved, error)) return B2SnapshotFailure(error, "client accepted a non-host author through the relay");
				auto real = saved.seats; real[1].state = NetSeatPresenceState::Disconnected;
				if (!round.Publish(real, 3000, error) || !round.Commit(1, error)) return false;
			}
			return true;
		}

		bool TestB2SeatSnapshotReplayAndPublication(std::string* error) {
			for (uint8_t count: {3, 4}) {
				B2SnapshotRound round;
				if (!round.Start(count, 44820 + count, error) || !round.Publish(B2SnapshotSeats(count), 1000, error)) return false;
				auto seats = B2SnapshotSeats(count); seats[1].state = NetSeatPresenceState::Disconnected;
				if (!round.Publish(seats, 2000, error)) return false;
				const auto saved = *round.latest[0];
				const auto reads = round.reads;
				const auto authority = round.Authority();
				const std::vector<std::pair<const char*, std::function<void(NetLockstepSeatSnapshot&)>>> regressions = {
					{"session", [](auto& s) { --s.sessionId; }},
					{"epoch", [](auto& s) { s.epoch[0] ^= 0x80; }},
					{"round", [](auto& s) { --s.roundId; }},
					{"duplicate revision", [&](auto& s) { s.revision = saved.revision; }},
					{"older revision", [&](auto& s) { s.revision = saved.revision - 1; }},
					{"time", [&](auto& s) { s.observedAtMs = saved.observedAtMs - 1; }},
					{"seat generation", [](auto& s) { --s.seats[1].seatGeneration; }},
					{"holder generation", [](auto& s) { --s.seats[1].holderGeneration; }},
					{"incarnation", [](auto& s) { --s.seats[1].incarnation; }},
					{"new holder without new seat generation", [](auto& s) { ++s.seats[1].holderGeneration; s.seats[1].incarnation = 0; }},
					{"peer remap", [](auto& s) { std::swap(s.seats[0].peerId, s.seats[1].peerId); }},
					{"stable-seat replacement", [](auto& s) { ++s.seats.back().stableSeat; }},
					{"omitted subject", [](auto& s) { s.seats.pop_back(); }},
					{"subject outside the round", [=](auto& s) { s.seats.back().peerId = count + 1; }},
				};
				for (const auto& [name, mutate]: regressions) {
					auto replay = saved; ++replay.revision; ++replay.observedAtMs; replay.seats[1].holderName = "Poison";
					mutate(replay);
					if (!round.SendToClients({replay}, error)) return false;
					if (round.reads != reads || round.Authority() != authority || !round.Equals(saved, error)) return B2SnapshotFailure(error, std::string("receiver accepted ") + name + " regression");
					if (replay.sessionId == saved.sessionId && replay.epoch == saved.epoch && replay.roundId == saved.roundId && replay.revision > saved.revision) {
						const size_t sent = round.transport[0].sent.size();
						if (round.peers[0]->PublishSeatSnapshot(replay.seats, replay.observedAtMs)) return B2SnapshotFailure(error, std::string("publisher accepted ") + name + " regression");
						round.Pump();
						if (round.reads != reads || round.transport[0].sent.size() != sent || !round.Equals(saved, error)) return B2SnapshotFailure(error, "refused publication mutated or sent a roster");
					}
				}
				seats[1].state = NetSeatPresenceState::Reconnecting;
				if (!round.Publish(seats, saved.observedAtMs, error)) return false;
				++seats[1].holderGeneration; ++seats[1].seatGeneration; seats[1].incarnation = 0;
				seats[1].state = NetSeatPresenceState::Substituted; seats[1].holderName = "New holder";
				if (!round.Publish(seats, 3000, error) || !round.Commit(0, error)) return false;
			}
			return true;
		}

		bool TestB2SeatSnapshotInitiallyComplete(std::string* error) {
			for (uint8_t count: {3, 4}) {
				B2SnapshotRound round;
				if (!round.Start(count, 44830 + count, error)) return false;
				auto incomplete = B2SnapshotSeats(count); incomplete.pop_back();
				if (round.peers[0]->PublishSeatSnapshot(incomplete, 1000)) return B2SnapshotFailure(error, "first publication omitted a configured human");
				auto packet = B2SnapshotPacket(count);
				packet.sessionId = round.config[0].sessionId; packet.epoch = round.config[0].seatPresenceEpoch;
				packet.roundId = round.peers[0]->GetRoundId(); packet.seats = incomplete;
				if (!round.SendToClients({packet}, error)) return false;
				for (uint8_t i = 0; i < count; ++i) if (round.latest[i] || round.views[i].GetSnapshot()) return B2SnapshotFailure(error, "first received roster omitted a configured human");
				if (!round.Publish(B2SnapshotSeats(count), 1000, error)) return false;
			}
			return true;
		}

		bool TestB2SeatSnapshotCoalesces(std::string* error) {
			for (uint8_t count: {3, 4}) {
				for (bool congestion: {false, true}) {
					B2SnapshotRound round;
					if (!round.Start(count, 44840 + count + (congestion ? 10 : 0), error) || !round.Publish(B2SnapshotSeats(count), 1000, error)) return false;
					const auto original = *round.latest[0];
					round.transport[0].sent.clear();
					for (uint8_t i = 1; i < count; ++i) round.transport[i].received.clear();
					std::vector<uint8_t> encoded;
					if (!EncodePacket({original}, encoded, error)) return false;
					LoopbackTransportConfig faults;
					if (congestion) {
						faults.sendBufferBytes = static_cast<uint32_t>(encoded.size()); faults.meterOnlyPeer = 1;
					} else {
						faults.refuseSendsToPeer = 1;
					}
					round.transport[0].SetFaultConfig(faults);
					if (congestion) {
						if (!EncodePacket({NetLockstepAck{1, 0, 0}}, encoded, error) || !round.transport[0].Send(1, NetTransportLane::ControlReliable, encoded, error)) return false;
					}
					auto seats = original.seats;
					for (unsigned update = 0; update < 64; ++update) {
						seats[1].state = update % 2 == 0 ? NetSeatPresenceState::Disconnected : NetSeatPresenceState::Reconnecting;
						if (!round.peers[0]->PublishSeatSnapshot(seats, 2000 + update)) return B2SnapshotFailure(error, "refused transport prevented publication");
					}
					round.Pump();
					if (!round.Running(error) || !round.latest[0] || round.latest[0]->revision != original.revision + 64 || round.latest[1] != std::optional<NetLockstepSeatSnapshot>{original} ||
					    !round.transport[1].received.empty() || round.peers[0]->HasPendingRelayWork() || round.peers[0]->GetStats().relayBacklogBytes != 0) return B2SnapshotFailure(error, "refused UI updates entered the simulation backlog or reached the blocked peer");
					for (uint8_t i = 2; i < count; ++i) if (round.latest[i] != round.latest[0] || round.transport[i].received.size() != 64) return B2SnapshotFailure(error, "one blocked destination stalled a healthy peer's roster");
					const auto refused = std::count_if(round.transport[0].sent.begin(), round.transport[0].sent.end(), [&](const auto& send) { return send.peer == 1 && !send.accepted && send.congested == congestion; });
					if (refused < 64) return B2SnapshotFailure(error, "transport refusal control was not exercised");
					const auto final = *round.latest[0];
					if (congestion) faults.drainBytesPerSecond = 1024 * 1024;
					else faults = {};
					round.transport[0].SetFaultConfig(faults);
					round.Pump(10);
					if (!round.Equals(final, error) || round.transport[1].received != std::vector<NetLockstepSeatSnapshot>{final}) return B2SnapshotFailure(error, "recovery sent queued intermediate rosters instead of one latest snapshot");
					const size_t sends = round.transport[0].sent.size();
					round.Pump(20);
					if (round.transport[0].sent.size() != sends) return B2SnapshotFailure(error, "delivered roster remained pending");
					round.transport[0].SetFaultConfig({});
					if (!round.Commit(0, error)) return false;
				}
			}
			return true;
		}

		bool TestB2SeatSnapshotResyncSeedsNewPeer(std::string* error) {
			for (uint8_t count: {3, 4}) {
				B2SnapshotRound round;
				if (!round.Start(count, 44860 + count, error) || !round.Commit(0, error)) return false;
				auto seats = B2SnapshotSeats(count);
				seats[1].state = NetSeatPresenceState::Substituted; seats[1].holderName = "Returning substitute";
				if (!round.Publish(seats, 1000, error)) return false;
				const auto previous = *round.latest[0];
				round.peers[0]->RequestResync("snapshot seed control");
				round.Pump(10);
				for (uint8_t i = 0; i < count; ++i) if (round.peers[i]->IsRunning()) return B2SnapshotFailure(error, "resync control did not end the old round");
				round.active[1] = false;
				round.transport[1].Stop(); round.Pump();
				if (!round.transport[1].Connect("loopback", round.port, error)) return false;
				NetLockstepCoordinator newcomer;
				round.peers[1] = &newcomer; round.views[1].Clear(); round.active[1] = true;
				round.hostSideId[1] = count;
				if (!round.StartRound(previous.roundId + 1, 10, error) || round.peers[0]->UsesTransportPeer(1) || !round.peers[0]->UsesTransportPeer(count)) return B2SnapshotFailure(error, "new round did not bind the replacement connection");
				if (!round.SendToClients({previous}, error)) return false;
				for (uint8_t i = 0; i < count; ++i) if (round.latest[i]) return B2SnapshotFailure(error, "old round seeded the new round");
				if (!round.Publish(seats, 2000, error) || round.latest[1]->revision != 1 || round.latest[1]->roundId != previous.roundId + 1) return B2SnapshotFailure(error, "new peer was not seeded by the complete new-round snapshot");
				const auto current = *round.latest[0];
				const auto reads = round.reads;
				auto ancient = previous; ancient.revision = UINT64_MAX; ancient.observedAtMs = UINT64_MAX;
				if (!round.SendToClients({ancient}, error) || round.reads != reads || !round.Equals(current, error)) return B2SnapshotFailure(error, "high-revision old round replaced the new seed");
				std::vector<uint8_t> bytes;
				auto forged = current; forged.senderPeerId = 2; ++forged.revision; ++forged.observedAtMs; forged.seats[1].holderName = "Old transport";
				if (!EncodePacket({forged}, bytes, error)) return false;
				const uint64_t heard = round.peers[0]->GetStats().peers.at(2).lastHeardMs;
				round.transport[0].injected.push_back({NetTransportEventType::PacketReceived, 1, NetTransportLane::ControlReliable, bytes, {}});
				round.Pump();
				if (round.reads != reads || round.peers[0]->GetStats().peers.at(2).lastHeardMs != heard || !round.Equals(current, error) || !round.Commit(10, error)) return B2SnapshotFailure(error, "old connection altered the new round");
			}
			return true;
		}

		bool TestB2SeatSnapshotDoesNotReviveDepartedTransport(std::string* error) {
			for (uint8_t count: {3, 4}) {
				B2SnapshotRound round;
				if (!round.Start(count, 44870 + count, error) || !round.Commit(0, error) || !round.Publish(B2SnapshotSeats(count), 1000, error)) return false;
				round.peers[1]->Leave("holder leaves"); round.active[1] = false; round.Pump();
				if (!round.Running(error) || round.peers[0]->UsesTransportPeer(1) || !round.transport[1].IsPeerConnected(1)) return B2SnapshotFailure(error, "leave did not remove only the binding while retaining the stale socket control");
				for (uint8_t i = 0; i < count; ++i) {
					if (!round.active[i]) continue;
					if (!round.peers[i]->IsPeerGoneAtFrame(2, 1)) return B2SnapshotFailure(error, "survivor did not apply the genuine leave");
					round.peers[i]->DeferStopsToTickBoundary();
				}
				auto seats = B2SnapshotSeats(count);
				seats[1].state = NetSeatPresenceState::Substituted; ++seats[1].holderGeneration; ++seats[1].seatGeneration;
				seats[1].incarnation = 0; seats[1].holderName = "Replacement awaiting resync";
				const auto authority = round.Authority();
				if (!round.Publish(seats, 2000, error) || round.Authority() != authority || round.peers[0]->UsesTransportPeer(1)) return B2SnapshotFailure(error, "substitution status granted the departed socket authority");
				const auto saved = *round.latest[0];
				const auto reads = round.reads;
				const uint64_t heard = round.peers[0]->GetStats().peers.at(2).lastHeardMs;
				for (uint8_t sender: {1, 2}) {
					auto forged = saved; forged.senderPeerId = sender; ++forged.revision; ++forged.observedAtMs;
					forged.seats[1].state = NetSeatPresenceState::Present;
					std::vector<uint8_t> bytes;
					if (!EncodePacket({forged}, bytes, error) || !round.transport[1].Send(1, NetTransportLane::ControlReliable, bytes, error)) return false;
					round.Pump();
					if (round.reads != reads || round.Authority() != authority || round.peers[0]->GetStats().peers.at(2).lastHeardMs != heard || !round.Equals(saved, error)) return B2SnapshotFailure(error, "departed socket changed its roster, liveness, or authority");
				}
				const NetLockstepStopReason terminal[] = {NetLockstepStopReason::Complete, NetLockstepStopReason::MissingFrameTimeout, NetLockstepStopReason::Desync,
					NetLockstepStopReason::ProtocolError, NetLockstepStopReason::PeerDisconnected, NetLockstepStopReason::InternalError, NetLockstepStopReason::ResyncRequested};
				for (const auto reason: terminal) {
					const NetLockstepPacket stop{NetLockstepStop{2, reason, 1, "stale terminal stop"}};
					std::vector<uint8_t> bytes;
					if (!EncodePacket(stop, bytes, error) || !round.transport[1].Send(1, NetTransportLane::ControlReliable, bytes, error)) return false;
					if (!round.SendToClients(stop, error) || !round.Running(error) || round.Authority() != authority) return B2SnapshotFailure(error, "departed peer's terminal stop changed a survivor");
					for (uint8_t i = 0; i < count; ++i) if (round.active[i] && round.peers[i]->HasPendingRecoveryStop()) return B2SnapshotFailure(error, "departed peer scheduled a deferred terminal stop");
				}
				for (uint8_t i = 2; i < count; ++i) if (round.peers[i]->GetStats().stopsFromLeftPeers != 7) return B2SnapshotFailure(error, "left-peer terminal-stop guard was not exercised on every survivor");
				if (!round.Commit(1, error)) return false;
				round.peers[0]->RequestResync("genuine host recovery");
				if (!round.peers[0]->HasPendingRecoveryStop() || !round.peers[0]->FinishSimulationTick(1)) return B2SnapshotFailure(error, "host recovery did not settle at its completed tick");
				round.Pump();
				for (uint8_t i = 2; i < count; ++i) if (!round.peers[i]->IsFailed() || round.peers[i]->GetStats().timeoutReason.find("ResyncRequested") == std::string::npos) return B2SnapshotFailure(error, "genuine host stop was suppressed with stale stops");
			}
			return true;
		}


		// A peer that ANNOUNCED its leave said it is not coming back, so nothing is held for it - the
		// wire tells the two apart, and every peer reads the same answer.
		bool TestAnnouncedLeaveHoldsNothing(std::string* error) {
			const uint16_t port = 43074;
			const uint64_t sessionId = 0x7000000000000074ULL;
			LoopbackTransport hostT, leaverT, stayerT;
			if (!hostT.StartHost(port, error) || !leaverT.Connect("loopback", port, error) || !stayerT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.timeoutMs = 5000;
				c.localPeerId = local;
				c.peerCount = 3;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			SeatStateStub hostStub;
			hostStub.held = true;
			ClientSeatStateProbe stayerProbe;
			NetLockstepCoordinator host, leaver, stayer;
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error) ||
			    !leaver.Start(leaverT, cfg(2, {{1, 1}}, false), error) ||
			    !stayer.Start(stayerT, cfg(3, {{1, 1}}, false), error)) {
				return false;
			}
			host.SetSeatStateSource(&QuerySeatStateStub, &hostStub);
			stayer.SetSeatStateSource(&QuerySeatStateAsAClient, &stayerProbe);
			uint64_t now = 0;
			auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
				for (const uint64_t until = now + forMs; now <= until; now += 5) {
					host.Tick(now);
					leaver.Tick(now);
					stayer.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					leaverT.AdvanceTimeMs(5);
					stayerT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive(2000, [&] { return host.IsRunning() && leaver.IsRunning() && stayer.IsRunning(); })) {
				*error = "the announced-leave round never started";
				return false;
			}
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !leaver.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error) ||
				    !stayer.QueueLocalInput(f, {MakeFrame(300, f + 1)}, {}, error)) {
					return false;
				}
			}
			NetLockstepReadyFrame ready;
			size_t committed = 0;
			if (!drive(2000, [&] {
					while (host.PopReadyFrame(ready)) {
						++committed;
					}
					while (stayer.PopReadyFrame(ready)) {
					}
					return committed >= 2;
				})) {
				*error = "the announced-leave round never committed a frame";
				return false;
			}
			leaver.Leave("bye");
			if (!drive(3000, [&] { return host.GetPeerLeaveFrames().count(2) != 0 && stayer.GetPeerLeaveFrames().count(2) != 0; })) {
				*error = "the announced leave never reached both survivors";
				return false;
			}
			auto gateAt = [](NetLockstepCoordinator& peer, uint64_t frame) {
				ScenarioRunner::SetLockstepCoordinator(&peer);
				ScenarioRunner::SetLockstepAppliedFrame(frame);
				const bool gate = ScenarioRunner::IsLockstepHoldingSeatForReclaim();
				ScenarioRunner::SetLockstepCoordinator(nullptr);
				return gate;
			};
			const uint64_t leaveFrame = host.GetPeerLeaveFrames().at(2);
			if (gateAt(host, leaveFrame) || gateAt(stayer, leaveFrame)) {
				*error = "an announced leave held a scripted outcome that should have applied at once";
				return false;
			}
			return true;
		}

		// B1: the substitution scenario at the resync save. One of two remotes drops, its seat is held
		// for its reclaim window, and the OTHER remote plays on - which is exactly what the four
		// substitution gates set up. A scripted outcome the absent player produced must wait for that
		// player, or the activity ends and the resync snapshot has no running game to save. The hold is
		// a frame deadline off the relayed leave notice, so it is the same fact on every peer.
		bool TestCoordinatorHeldSeatWithASurvivor(std::string* error) {
			const uint16_t port = 43062;
			const uint64_t sessionId = 0x7000000000000062ULL;
			LoopbackTransport hostT, leaverT, stayerT;
			if (!hostT.StartHost(port, error) || !leaverT.Connect("loopback", port, error) || !stayerT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.timeoutMs = 5000;
				c.localPeerId = local;
				c.peerCount = 3;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			SeatStateStub stub;
			stub.held = true;
			NetLockstepCoordinator host, leaver, stayer;
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error) ||
			    !leaver.Start(leaverT, cfg(2, {{1, 1}}, false), error) ||
			    !stayer.Start(stayerT, cfg(3, {{1, 1}}, false), error)) {
				return false;
			}
			host.SetSeatStateSource(&QuerySeatStateStub, &stub);
			uint64_t now = 0;
			auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
				for (const uint64_t until = now + forMs; now <= until; now += 5) {
					host.Tick(now);
					leaver.Tick(now);
					stayer.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					leaverT.AdvanceTimeMs(5);
					stayerT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive(2000, [&] { return host.IsRunning() && leaver.IsRunning() && stayer.IsRunning(); })) {
				*error = "the three-peer round never started";
				return false;
			}
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !leaver.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error) ||
				    !stayer.QueueLocalInput(f, {MakeFrame(300, f + 1)}, {}, error)) {
					return false;
				}
			}
			NetLockstepReadyFrame ready;
			size_t committed = 0;
			if (!drive(2000, [&] {
					while (host.PopReadyFrame(ready)) {
						++committed;
					}
					return committed >= 2;
				})) {
				*error = "the three-peer round never committed a frame";
				return false;
			}

			leaverT.Stop();
			if (!drive(2000, [&] { return host.GetPeerLeaveFrames().count(2) != 0; })) {
				*error = "the drop was never adjudicated as a leave";
				return false;
			}
			if (host.GetState() != NetLockstepState::Running) {
				*error = "the round stopped although a peer is still playing";
				return false;
			}
			// The round with nobody left is a different question, and its answer must not move: one
			// remote is still here, so this stays false and ownership behaviour is untouched.
			if (host.IsHoldingSeatForReclaim()) {
				*error = "a round with a surviving remote reported itself as holding for reclaim";
				return false;
			}
			const uint64_t leaveFrame = host.GetPeerLeaveFrames().at(2);
			if (!host.IsSeatHeldForReclaimAtFrame(leaveFrame)) {
				*error = "the dropped peer's seat is inside its window but the round does not say so";
				return false;
			}
			if (!host.IsSeatHeldForReclaimAtFrame(leaveFrame + NetLockstepCoordinator::c_ReclaimHoldFrames)) {
				*error = "the hold released at the old frame deadline";
				return false;
			}
			ScenarioRunner::SetLockstepCoordinator(&host);
			struct ClearCoordinator {
				~ClearCoordinator() { ScenarioRunner::SetLockstepCoordinator(nullptr); }
			} clearCoordinator;
			ScenarioRunner::SetLockstepAppliedFrame(leaveFrame);
			if (!ScenarioRunner::IsLockstepHoldingSeatForReclaim()) {
				*error = "the activity gate would let a scripted outcome end the match under a returning player";
				return false;
			}
			if (!ActivityMan::ScriptedEndIsDeferred(true, true, ScenarioRunner::IsLockstepHoldingSeatForReclaim())) {
				*error = "the scripted end was not deferred while a seat is held";
				return false;
			}

			ScenarioRunner::SetLockstepAppliedFrame(leaveFrame + NetLockstepCoordinator::c_ReclaimHoldFrames);
			if (!ScenarioRunner::IsLockstepHoldingSeatForReclaim()) {
				*error = "the hold released at the old frame deadline";
				return false;
			}
			host.ResolveHeldSeat(2, NetLockstepHoldResolution::Expired, now);
			stayer.Tick(now);
			host.Tick(now);
			if (host.AnyDroppedSeatHeld() || ScenarioRunner::IsLockstepHoldingSeatForReclaim()) {
				*error = "the hold survived Expired";
				return false;
			}
			if (ActivityMan::ScriptedEndIsDeferred(true, true, ScenarioRunner::IsLockstepHoldingSeatForReclaim()) ||
			    ActivityMan::ScriptedEndIsDeferred(false, true, true)) {
				*error = "the end was deferred with no held seat, or an engine teardown was held back";
				return false;
			}
			return true;
		}

		// H4 §4: a 1v1 whose only remote DROPS holds its seat for the reclaim window instead of ending,
		// so the returner has a match to come back to. A clean leave with nobody left still ends at once.
		bool TestCoordinatorDroppedSeatHold(std::string* error) {
			uint16_t port = 43020;
			bool holdingBeforeDrop = false;
			bool holdingDuringHold = false;
			bool holdingAfterWindow = false;
			bool stillUsesDeadTransport = false;
			auto runDrop = [&](bool holdSeat, bool fenceTransport, bool cleanLeave, NetLockstepState& outState,
			                   size_t& outLeaves, std::string& outReason, uint64_t& outFramesAlone) {
				++port;
				const uint64_t sessionId = 0x7000000000000020ULL + port;
				LoopbackTransport hostT, clientT;
				std::string ignored;
				if (!hostT.StartHost(port, &ignored) || !clientT.Connect("loopback", port, &ignored)) {
					return false;
				}
				auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
					NetLockstepConfig c;
					c.sessionId = sessionId;
					c.timeoutMs = 5000;
					c.localPeerId = local;
					c.peerCount = 2;
					c.remoteTransportPeerIds = std::move(transports);
					c.relayToOtherPeers = relay;
					c.scenario = "LockstepSelfTest";
					c.ownershipPolicy = "unique-id-split";
					return c;
				};
				SeatStateStub stub;
				stub.held = holdSeat;
				NetLockstepCoordinator host, client;
				if (!host.Start(hostT, cfg(1, {{2, 1}}, true), &ignored) || !client.Start(clientT, cfg(2, {{1, 1}}, false), &ignored)) {
					return false;
				}
				host.SetSeatStateSource(&QuerySeatStateStub, &stub);
				uint64_t now = 0;
				auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
					for (const uint64_t until = now + forMs; now <= until; now += 5) {
						host.Tick(now);
						client.Tick(now);
						if (done()) {
							return true;
						}
						hostT.AdvanceTimeMs(5);
						clientT.AdvanceTimeMs(5);
					}
					return false;
				};
				if (!drive(2000, [&] { return host.IsRunning() && client.IsRunning(); })) {
					return false;
				}
				for (uint64_t f = 0; f < 2; ++f) {
					if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, &ignored) ||
					    !client.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, &ignored)) {
						return false;
					}
				}
				NetLockstepReadyFrame ready;
				size_t committed = 0;
				if (!drive(2000, [&] {
						while (host.PopReadyFrame(ready)) {
							++committed;
						}
						return committed >= 2;
					})) {
					return false;
				}
				// The drop: the transport goes away with no notice. A clean leave announces itself first.
				holdingBeforeDrop = host.IsHoldingSeatForReclaim();
				stub.fenced = fenceTransport ? static_cast<NetPeerId>(1) : c_InvalidNetPeerId;
				if (cleanLeave) {
					client.Leave("bye");
					drive(200, [] { return false; });
				}
				clientT.Stop();
				drive(200, [] { return false; });
				// Whatever the host decided, it must be able to keep producing frames on its own.
				outFramesAlone = 0;
				for (uint64_t f = 2; f < 5; ++f) {
					if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, &ignored)) {
						break;
					}
				}
				drive(200, [&] {
					while (host.PopReadyFrame(ready)) {
						++outFramesAlone;
					}
					return false;
				});
				outState = host.GetState();
				outLeaves = host.GetPeerLeaveFrames().size();
				outReason = host.GetStats().timeoutReason;
				// A5: the activity gate reads exactly this - the round is alive only for a held seat.
				holdingDuringHold = host.IsHoldingSeatForReclaim();
				// A6: whatever the round decided, it must stop naming a transport that is gone.
				stillUsesDeadTransport = host.UsesTransportPeer(static_cast<NetPeerId>(1));
				if (outState == NetLockstepState::Running && !cleanLeave && !fenceTransport) {
					host.ResolveHeldSeat(2, NetLockstepHoldResolution::Expired, now + 5);
					host.Tick(now + 5);
					outState = host.GetState();
					outReason = host.GetStats().timeoutReason;
					holdingAfterWindow = host.IsHoldingSeatForReclaim();
				}
				return true;
			};

			NetLockstepState state = NetLockstepState::Idle;
			size_t leaves = 0;
			std::string reason;
			uint64_t framesAlone = 0;

			// Held: commits freeze until Expired, then a last-player 1v1 ends.
			if (!runDrop(true, false, false, state, leaves, reason, framesAlone)) {
				*error = "the held-seat drop fixture did not run";
				return false;
			}
			if (leaves != 1) {
				*error = "a held drop did not stop requiring the dropped peer's frames";
				return false;
			}
			if (framesAlone != 0) {
				*error = "the host committed frames while a dropped seat was held";
				return false;
			}
			if (state != NetLockstepState::Stopped || reason.rfind("PeerLeft:", 0) != 0) {
				*error = "the round did not end once Expired resolved the last seat: " + reason;
				return false;
			}
			if (holdingBeforeDrop || !holdingDuringHold || holdingAfterWindow) {
				*error = "the held-seat pause was not visible to the activity gate";
				return false;
			}

			uint64_t controlFrames = 0;
			if (!runDrop(false, false, false, state, leaves, reason, controlFrames)) {
				*error = "the unheld-seat control did not run";
				return false;
			}
			if (state != NetLockstepState::Stopped || reason.rfind("PeerLeft:", 0) != 0) {
				*error = "an unheld 1v1 drop no longer ends after Expired: " + reason;
				return false;
			}
			if (!holdingDuringHold) {
				*error = "an unresolved dropped seat did not pause the round";
				return false;
			}
			if (controlFrames != 0) {
				*error = "the host committed frames after an unheld drop";
				return false;
			}

			// A clean leave with nobody left ends the match at once even while the seat would be held.
			if (!runDrop(true, false, true, state, leaves, reason, framesAlone)) {
				*error = "the clean-leave fixture did not run";
				return false;
			}
			if (state != NetLockstepState::Stopped || reason.rfind("PeerLeft:", 0) != 0) {
				*error = "a clean 1v1 leave no longer ends the match: " + reason;
				return false;
			}
			if (holdingDuringHold) {
				*error = "an announced leave still reported the round as holding its seat";
				return false;
			}

			// A superseded incarnation's socket closing is not a leave at all: the seat's live holder is
			// another transport, so the round keeps requiring it.
			if (!runDrop(false, true, false, state, leaves, reason, framesAlone)) {
				*error = "the fenced-transport fixture did not run";
				return false;
			}
			if (leaves != 0 || state != NetLockstepState::Running) {
				*error = "a fenced transport's disconnect was adjudicated as a leave";
				return false;
			}
			if (holdingDuringHold) {
				*error = "a round with nobody gone reported itself as holding a seat";
				return false;
			}
			if (stillUsesDeadTransport) {
				*error = "the round kept naming a superseded incarnation's transport";
				return false;
			}
			return true;
		}

		// The host's silence budget and the seat hold answer different questions, and a round that
		// loses every remote is where the two meet: adjudication decides whether to keep WAITING for a
		// peer, the hold decides whether the round may END because nobody is coming back. A peer the
		// host called gone on its own budget still holds its seat.
		bool TestCoordinatorAdjudicatedPeerKeepsItsSeat(std::string* error) {
			StarFixture fx;
			const uint32_t timeoutMs = 400;
			SeatStateStub stub;
			stub.held = true;
			stub.heldPeerId = 3; // Only the peer that wedges is holding a ticket.
			if (!fx.Start(43016, 0x7000000000000016ULL, timeoutMs, error)) {
				return false;
			}
			fx.host.SetSeatStateSource(&QuerySeatStateStub, &stub);
			if (!fx.Drive(true, true, true, [&] { return fx.Running(); }, 2000)) {
				*error = "adjudicated-seat fixture did not reach Running";
				return false;
			}
			std::vector<uint64_t> hostReady;
			if (!fx.DriveProducing(true, [&] {
					fx.Collect(fx.host, hostReady);
					return hostReady.size() >= 2;
				}, fx.now + timeoutMs)) {
				*error = "adjudicated-seat fixture did not get the round moving";
				return false;
			}
			// B wedges: the host calls it gone on its own budget rather than waiting on its socket.
			if (!fx.DriveProducing(false, [&] { return fx.host.GetPeerLeaveFrames().count(3) != 0; }, fx.now + 4 * timeoutMs)) {
				*error = "the host kept waiting for the wedged peer: " + fx.host.BuildReportJson();
				return false;
			}
			if (fx.host.GetStats().peersDroppedSilent != 1) {
				*error = "the wedged peer was not adjudicated: " + fx.host.BuildReportJson();
				return false;
			}
			fx.Collect(fx.host, hostReady);
			// A's socket goes away too, so no remote is left - the round stays paused until each held seat is resolved.
			fx.clientAT.Stop();
			fx.Collect(fx.host, hostReady);
			const size_t committedBefore = hostReady.size();
			fx.DriveProducing(false, [&] {
				fx.Collect(fx.host, hostReady);
				return false;
			}, fx.now + 4 * timeoutMs);
			if (hostReady.size() != committedBefore) {
				*error = "the host committed " + std::to_string(hostReady.size() - committedBefore) +
				         " frames while dropped seats were held: " + fx.host.BuildReportJson();
				return false;
			}
			if (!fx.host.IsRunning()) {
				*error = "a round holding an adjudicated peer's seat ended with the last remote: " + fx.host.GetStats().timeoutReason;
				return false;
			}
			fx.host.ResolveHeldSeat(3, NetLockstepHoldResolution::Expired, fx.now + 5);
			fx.host.ResolveHeldSeat(2, NetLockstepHoldResolution::Expired, fx.now + 5);
			fx.host.Tick(fx.now + 5);
			if (fx.host.GetState() != NetLockstepState::Stopped || fx.host.GetStats().timeoutReason.rfind("PeerLeft:", 0) != 0) {
				*error = "the round did not end once Expired resolved the last seats: " + fx.host.GetStats().timeoutReason;
				return false;
			}
			return true;
		}

		// A6: a seat inside its reclaim window still has a player, so its units keep playing under the
		// relay host instead of standing down to be shot where they stand - which is what emptied the
		// returner's team before the resync snapshot was ever taken.
		bool TestCoordinatorHeldSeatKeepsPlaying(std::string* error) {
			const uint16_t port = 43040;
			const uint64_t sessionId = 0x7000000000000040ULL;
			LoopbackTransport hostT, clientT;
			if (!hostT.StartHost(port, error) || !clientT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.timeoutMs = 5000;
				c.localPeerId = local;
				c.peerCount = 2;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				c.matchConfig.hostPeerId = 1;
				c.matchConfig.players = {{1, 0, false, "Host"}, {2, 1, false, "Client"}};
				return c;
			};
			SeatStateStub stub;
			NetLockstepCoordinator host, client;
			if (!host.Start(hostT, cfg(1, {{2, 1}}, true), error) || !client.Start(clientT, cfg(2, {{1, 1}}, false), error)) {
				return false;
			}
			host.SetSeatStateSource(&QuerySeatStateStub, &stub);
			uint64_t now = 0;
			auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
				for (const uint64_t until = now + forMs; now <= until; now += 5) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					clientT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive(2000, [&] { return host.IsRunning() && client.IsRunning(); })) {
				*error = "the held-seat ownership fixture did not reach Running";
				return false;
			}

			// A round that has committed nothing may not be judged yet: after a resync relaunch the
			// ledgered reseat rides the first committed frame.
			if (host.HasCommittedAFrame()) {
				*error = "a round reported a committed frame before it had one";
				return false;
			}
			const int64_t clientActor = 4242;
			if (host.ResolveActorOwner(clientActor, 1, false) != 2) {
				*error = "the client's team did not resolve to the client before the drop";
				return false;
			}
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !client.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error)) {
					return false;
				}
			}
			NetLockstepReadyFrame ready;
			size_t committed = 0;
			if (!drive(2000, [&] {
					while (host.PopReadyFrame(ready)) {
						++committed;
					}
					return committed >= 2;
				})) {
				*error = "the held-seat ownership fixture never committed a frame";
				return false;
			}
			if (!host.HasCommittedAFrame()) {
				*error = "a round that committed two frames still reported none";
				return false;
			}

			// The drop, with the seat held: the client's units become the host's to play, and nothing
			// stands them down.
			stub.held = true;
			clientT.Stop();
			drive(200, [] { return false; });
			if (host.GetPeerLeaveFrames().size() != 1 || !host.IsHoldingSeatForReclaim()) {
				*error = "the held drop did not put the round in its reclaim window";
				return false;
			}
			if (host.ResolveActorOwner(clientActor, 1, false) != 1) {
				*error = "a held seat's units did not fall to the relay host";
				return false;
			}
			if (host.IsActorOwnerGone(clientActor, 1, false, host.GetStats().nextFrame)) {
				*error = "a held seat's units were stood down while their player could still return";
				return false;
			}
			if (!host.IsLocalActor(clientActor, 1, false)) {
				*error = "the relay host did not take the held seat's units as its own";
				return false;
			}
			// The host's own units are untouched by any of this.
			if (host.ResolveActorOwner(7777, 0, false) != 1 || host.IsActorOwnerGone(7777, 0, false, host.GetStats().nextFrame)) {
				*error = "the hold moved the host's own units";
				return false;
			}

			stub.held = false;
			if (host.IsActorOwnerGone(clientActor, 1, false, host.GetStats().nextFrame)) {
				*error = "clearing the admission stub stood units down before Expired";
				return false;
			}
			host.ResolveHeldSeat(2, NetLockstepHoldResolution::Expired, now + 5);
			host.Tick(now + 5);
			if (host.ResolveActorOwner(clientActor, 1, false) != 0) {
				*error = "a released seat's units still had an owner";
				return false;
			}
			if (!host.IsActorOwnerGone(clientActor, 1, false, host.GetStats().nextFrame)) {
				*error = "a released seat's units were not stood down";
				return false;
			}
			return true;
		}

		// The match service holds its own mutex across the whole session pump, and the drop's ownership
		// census runs inside it - so a seat read from an ownership query locks that mutex on the thread
		// that already owns it. This raises what MSVC's std::mutex raises there instead of re-locking it,
		// which is undefined rather than observable.
		struct ServiceLockedSeatStateStub {
			SeatStateStub seat;
			std::mutex mutex;
			std::thread::id owner;
			uint32_t reads = 0;
			uint32_t reentries = 0;
		};

		// Stands in for NetMatchService::PumpSessionEvents holding m_Mutex for the length of the pump.
		struct ServiceLockScope {
			explicit ServiceLockScope(ServiceLockedSeatStateStub& stub) :
			    m_Stub(stub), m_Lock(stub.mutex) { m_Stub.owner = std::this_thread::get_id(); }
			~ServiceLockScope() { m_Stub.owner = std::thread::id{}; }
			ServiceLockedSeatStateStub& m_Stub;
			std::lock_guard<std::mutex> m_Lock;
		};

		NetLockstepSeatState QuerySeatStateUnderServiceLock(void* context, uint8_t peerId, NetPeerId transportPeerId) {
			auto* stub = static_cast<ServiceLockedSeatStateStub*>(context);
			if (stub->owner == std::this_thread::get_id()) {
				++stub->reentries;
				throw std::system_error(std::make_error_code(std::errc::resource_deadlock_would_occur), "seat state read under the match service's lock");
			}
			const std::lock_guard<std::mutex> lock(stub->mutex);
			++stub->reads;
			return QuerySeatStateStub(&stub->seat, peerId, transportPeerId);
		}

		// Models NetMatchService::QuerySeatState's own early return on a client (`!m_IsHost` at
		// NetMatchService.cpp:1158-1160): a client's admission plane knows nothing about seats, so it
		// answers the default. The read count is what stops the arm below passing vacuously.
		NetLockstepSeatState QueryNonHostSeatState(void* context, uint8_t peerId, NetPeerId transportPeerId) {
			(void)peerId;
			(void)transportPeerId;
			++*static_cast<uint32_t*>(context);
			return NetLockstepSeatState{};
		}

		// followup-5 item 6: IsHoldingSeatForReclaim() is answered from the HOST's admission plane and
		// nowhere else, and three sim-visible decisions read it - who owns a leaver's units, whether they
		// stand down, and whether they are ours to produce frames for. If a client could be in the round
		// while the host holds, the two would resolve differently and the sims would part.
		//
		// They cannot, and this measures why rather than asserting it: the predicate also requires every
		// remote to have left (NetLockstep.cpp:3013-3016, `m_PeerLeaveFrames.size() >= m_RemotePeerIds.size()`),
		// so the hold and a peer that can disagree with it are mutually exclusive. Arm 1 keeps a survivor
		// in the round and compares both peers' answers at every frame of a full hold window; arm 2 takes
		// the survivor away, shows the answers really do part once the hold engages, and shows there is no
		// longer anybody in the round to see it. Without arm 2 the first would prove nothing.
		bool TestHeldSeatOwnershipAgreesAcrossPeers(std::string* error) {
			const uint16_t port = 43044;
			const uint64_t sessionId = 0x7000000000000044ULL;
			LoopbackTransport hostT, aT, bT;
			if (!hostT.StartHost(port, error) || !aT.Connect("loopback", port, error) || !bT.Connect("loopback", port, error)) {
				return false;
			}
			// The leaver is alone on its team and the stayer is on another: this is the ONLY shape that
			// reaches the branch reading the hold, because a surviving teammate answers before it.
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.timeoutMs = 5000;
				c.localPeerId = local;
				c.peerCount = 3;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "team-owner";
				c.matchConfig.hostPeerId = 1;
				c.matchConfig.peerCount = 3;
				c.matchConfig.players = {{1, 0, false, "Host"}, {2, 1, false, "A"}, {3, 2, false, "B"}};
				return c;
			};
			SeatStateStub hostSeats;
			hostSeats.held = true; // The admission plane holds every dropped seat for its window.
			uint32_t stayerSeatReads = 0;
			NetLockstepCoordinator host, clientA, clientB;
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error) ||
			    !clientA.Start(aT, cfg(2, {{1, 1}}, false), error) ||
			    !clientB.Start(bT, cfg(3, {{1, 1}}, false), error)) {
				return false;
			}
			host.SetSeatStateSource(&QuerySeatStateStub, &hostSeats);
			clientB.SetSeatStateSource(&QueryNonHostSeatState, &stayerSeatReads);

			uint64_t now = 0;
			auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
				for (const uint64_t until = now + forMs; now <= until; now += 5) {
					host.Tick(now);
					clientA.Tick(now);
					clientB.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					aT.AdvanceTimeMs(5);
					bT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive(4000, [&] { return host.IsRunning() && clientA.IsRunning() && clientB.IsRunning(); })) {
				*error = "the held-seat ownership fixture did not reach Running";
				return false;
			}
			ControllerFrame frame;
			frame.stateMask = 1;
			for (uint64_t f = 0; f < 2; ++f) {
				frame.actorUniqueID = 100;
				if (!host.QueueLocalInput(f, {frame}, {}, error)) {
					return false;
				}
				frame.actorUniqueID = 200;
				if (!clientA.QueueLocalInput(f, {frame}, {}, error)) {
					return false;
				}
				frame.actorUniqueID = 300;
				if (!clientB.QueueLocalInput(f, {frame}, {}, error)) {
					return false;
				}
			}
			NetLockstepReadyFrame ready;
			size_t committed = 0;
			if (!drive(4000, [&] {
					while (host.PopReadyFrame(ready)) {
						++committed;
					}
					return committed >= 2;
				})) {
				*error = "the held-seat ownership fixture never committed a frame";
				return false;
			}

			const int64_t leaverActor = 4242;
			// Arm 1: peer 2 drops, peer 3 stays. Both peers still in the round answer every frame of a
			// full hold window, and must answer the same.
			aT.Stop();
			if (!drive(2000, [&] { return host.GetPeerLeaveFrames().count(2) != 0 && clientB.GetPeerLeaveFrames().count(2) != 0; })) {
				*error = "the survivor never learned of the drop, so the arm compares nothing";
				return false;
			}
			const uint64_t windowMs = 20'000;
			uint32_t samples = 0;
			for (const uint64_t until = now + windowMs; now <= until; now += 20) {
				host.Tick(now);
				clientB.Tick(now);
				const uint64_t at = host.GetStats().nextFrame;
				const uint8_t hostOwner = host.ResolveActorOwner(leaverActor, 1, false);
				const uint8_t stayerOwner = clientB.ResolveActorOwner(leaverActor, 1, false);
				const bool hostGone = host.IsActorOwnerGone(leaverActor, 1, false, at);
				const bool stayerGone = clientB.IsActorOwnerGone(leaverActor, 1, false, at);
				if (hostOwner != stayerOwner || hostGone != stayerGone) {
					*error = "the held-seat ownership decision disagrees across peers still in the round: host owner=" +
					         std::to_string(hostOwner) + " gone=" + std::to_string(hostGone) + " stayer owner=" +
					         std::to_string(stayerOwner) + " gone=" + std::to_string(stayerGone) + " at frame " + std::to_string(at);
					return false;
				}
				// The third consumer: local production. It differs by construction - it is the owner
				// compared with this peer's own id - so it must follow the owner both peers agree on.
				if (host.IsLocalActor(leaverActor, 1, false) != (hostOwner == 1) ||
				    clientB.IsLocalActor(leaverActor, 1, false) != (stayerOwner == 3)) {
					*error = "local production did not follow the owner the peers agreed on";
					return false;
				}
				// The branch under test was actually reached: the leaver's team has no survivor, so both
				// peers fell through to the hold and both were told there is none.
				if (hostOwner != 0 || !hostGone) {
					*error = "the leaver's ownerless team did not reach the hold branch";
					return false;
				}
				if (host.IsHoldingSeatForReclaim() || clientB.IsHoldingSeatForReclaim()) {
					*error = "the round reported a reclaim hold while a peer was still in it";
					return false;
				}
				++samples;
				hostT.AdvanceTimeMs(20);
				bT.AdvanceTimeMs(20);
			}
			if (samples < 500) {
				*error = "the hold window was not actually sampled: samples=" + std::to_string(samples) +
				         " stayer_seat_reads=" + std::to_string(stayerSeatReads);
				return false;
			}
			// And the host WAS told the seat is held throughout - it is the every-remote-gone half of the
			// predicate that is false, not the seat half.
			if (!hostSeats.held) {
				*error = "the host's admission plane stopped holding the seat mid-window";
				return false;
			}

			// Arm 2: the survivor goes too. Now the hold engages, the answers really do part - and there
			// is nobody left in the round to hold the other one.
			const uint64_t stayerFramesBeforeTheHold = clientB.GetStats().framesAccepted;
			bT.Stop();
			if (!drive(2000, [&] { return host.GetPeerLeaveFrames().count(3) != 0 && host.IsHoldingSeatForReclaim(); })) {
				*error = "the round with every remote gone never entered its reclaim hold";
				return false;
			}
			const uint64_t heldAt = host.GetStats().nextFrame;
			if (host.ResolveActorOwner(leaverActor, 1, false) != 1 || host.IsActorOwnerGone(leaverActor, 1, false, heldAt)) {
				*error = "a held seat's units did not fall to the relay host";
				return false;
			}
			// The disagreement is real, and it is exactly the one the brief names: the host plays the
			// leaver's units, the client resolves them to nobody and disables their controllers.
			if (clientB.ResolveActorOwner(leaverActor, 1, false) != 0 || !clientB.IsActorOwnerGone(leaverActor, 1, false, heldAt)) {
				*error = "the disagreement this case exists to detect did not appear even with the hold engaged: stayer owner=" +
				         std::to_string(clientB.ResolveActorOwner(leaverActor, 1, false)) + " gone=" +
				         std::to_string(clientB.IsActorOwnerGone(leaverActor, 1, false, heldAt));
				return false;
			}
			// And why that costs nothing: the predicate needs every remote gone, so the peer that would
			// have disagreed has left the round and produces no further frame.
			if (!host.IsPeerGoneAtFrame(2, heldAt) || !host.IsPeerGoneAtFrame(3, heldAt)) {
				*error = "the round held a seat while a remote was still present";
				return false;
			}
			// And what "left the round" has to mean for a sim: it commits no further frame, so there is no
			// tick at which the two answers could be applied to anything. Measured, not asserted from the
			// state name - the round object stays Running until its own timeout, it is simply starved.
			drive(2000, [] { return false; });
			if (clientB.GetStats().framesAccepted != stayerFramesBeforeTheHold) {
				*error = "the peer whose answer differs committed " +
				         std::to_string(clientB.GetStats().framesAccepted - stayerFramesBeforeTheHold) +
				         " more frames while the host held the seat";
				return false;
			}
			return true;
		}

		// A6: the drop of the last remote with nobody left on its team is the branch that asks whether the
		// seat is held - and on the host it is asked from inside the pump that holds the service's lock.
		// The round must answer that from what it already knows, never by asking back.
		bool TestSeatStateNeverReadUnderTheServiceLock(std::string* error, bool sharedTeam = false) {
			const uint16_t port = 43042;
			const uint64_t sessionId = 0x7000000000000042ULL;
			LoopbackTransport hostT, clientT;
			if (!hostT.StartHost(port, error) || !clientT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.timeoutMs = 200;
				c.localPeerId = local;
				c.peerCount = 2;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				c.matchConfig.hostPeerId = 1;
				c.matchConfig.players = {{1, 0, false, "Host"}, {2, static_cast<uint8_t>(sharedTeam ? 0 : 1), false, "Client"}};
				return c;
			};
			ServiceLockedSeatStateStub stub;
			stub.seat.held = true;
			NetLockstepCoordinator host, client;
			if (!host.Start(hostT, cfg(1, {{2, 1}}, true), error) || !client.Start(clientT, cfg(2, {{1, 1}}, false), error)) {
				return false;
			}
			host.SetSeatStateSource(&QuerySeatStateUnderServiceLock, &stub);
			uint64_t now = 0;
			auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
				for (const uint64_t until = now + forMs; now <= until; now += 5) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					clientT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive(2000, [&] { return host.IsRunning() && client.IsRunning(); })) {
				*error = "the service-lock fixture did not reach Running";
				return false;
			}
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !client.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error)) {
					return false;
				}
			}
			NetLockstepReadyFrame ready;
			size_t committed = 0;
			if (!drive(2000, [&] {
					while (host.PopReadyFrame(ready)) {
						++committed;
					}
					return committed >= 2;
				})) {
				*error = "the service-lock fixture never committed a frame";
				return false;
			}
			// The drop: the only remote goes away and its team has no other human, so the ownership
			// fallback has to decide whether the seat is held.
			clientT.Stop();
			if (!drive(600, [&] { return host.GetPeerLeaveFrames().count(2) != 0; })) {
				*error = "the drop was never adjudicated as a leave";
				return false;
			}
			if (!host.IsHoldingSeatForReclaim()) {
				*error = "the held drop did not put the round in its reclaim window";
				return false;
			}
			for (uint64_t f = 2; f < 5; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error)) {
					return false;
				}
			}

			const int64_t clientActor = 4242;
			const int32_t clientTeam = sharedTeam ? 0 : 1;
			uint32_t pumps = 0;
			uint8_t censusOwner = 0;
			bool censusOwnerGone = true;
			bool censusLocal = false;
			bool censusHolding = false;
			// The world the drop's census walks. MovableMan resolves each of these on the line below.
			const std::vector<std::pair<int64_t, int32_t>> world = {{clientActor, clientTeam}, {4243, clientTeam}, {7777, 0}};
			NetReconnectLedger ledger;
			std::vector<int64_t> ledgered;
			std::vector<int64_t> restored;
			auto takeCensus = [&] {
				std::vector<NetH4LedgerActor> census;
				for (const auto& [uid, team]: world) {
					// MovableMan::BuildLockstepOwnershipCensus's own line, per settled actor.
					census.push_back({uid, team, ScenarioRunner::GetLockstepDropTimeActorOwner(uid, team, false), true});
				}
				return census;
			};
			// The census the drop takes, run from inside the service's critical section exactly as
			// PumpSessionEvents runs it - and driven from the production wait loop, not a hand-rolled one.
			ScenarioRunner::SetLockstepCoordinator(&host);
			if (sharedTeam) {
				ScenarioRunner::SetLockstepControlOverride(clientActor, 2);
				ScenarioRunner::PurgeLockstepControlOverridesForGonePeers(host.GetStats().nextFrame);
				if (ScenarioRunner::GetLockstepActorOwner(clientActor, clientTeam, false) != 1 ||
				    ScenarioRunner::GetLockstepDropTimeActorOwner(clientActor, clientTeam, false) != 2) {
					ScenarioRunner::SetLockstepCoordinator(nullptr);
					*error = "co-op handoff cleanup lost the owner before admission could record its drop";
					return false;
				}
			}
			const auto pump = [&] {
				const ServiceLockScope serviceLock(stub);
				++pumps;
				censusOwner = host.ResolveActorOwner(clientActor, clientTeam, false);
				censusOwnerGone = host.IsActorOwnerGone(clientActor, clientTeam, false, host.GetStats().nextFrame);
				censusLocal = host.IsLocalActor(clientActor, clientTeam, false);
				censusHolding = host.IsHoldingSeatForReclaim();
				const std::vector<NetH4LedgerActor> dropCensus = takeCensus();
				ledgered = NetReconnectLedger::CollectOwnedActorUIDs(dropCensus, 2);
				ledger.RecordDrop(0, 2, clientTeam, host.GetStats().nextFrame, ledgered);
				restored = ledger.BuildRestoration(0, NetMatchMode::PvPSkirmish, takeCensus());
			};
			ScenarioRunner::SetSessionPump(pump);
			std::string reentry;
			try {
				host.Tick(now + 5);
				pump();
			} catch (const std::system_error& fault) {
				reentry = fault.what();
			}
			ScenarioRunner::SetSessionPump(nullptr);
			ScenarioRunner::SetLockstepCoordinator(nullptr);

			if (!reentry.empty() || stub.reentries != 0) {
				*error = "the drop's ownership census read the seat state under the match service's lock: " + reentry;
				return false;
			}
			if (pumps == 0) {
				*error = "the census never ran inside the service's lock";
				return false;
			}
			if (stub.reads == 0) {
				*error = "the round never read the seat state at all";
				return false;
			}
			// A6's semantics, seen from where the host actually asks: the held seat's units are the
			// host's to play and nothing stands them down.
			if (censusOwner != 1 || censusOwnerGone || !censusLocal || !censusHolding) {
				*error = "the census did not see the held seat's units fall to the relay host";
				return false;
			}
			// And what the ledger frames produced there: the leaver's own units, not the ones the leave
			// renamed, and a restoration that hands exactly those back.
			const std::vector<int64_t> expected = sharedTeam ? std::vector<int64_t>{clientActor} : std::vector<int64_t>{clientActor, 4243};
			if (ledgered != expected || restored != expected) {
				*error = "the drop ledgered " + std::to_string(ledgered.size()) + " units and restored " +
				         std::to_string(restored.size()) + " under the service's lock";
				return false;
			}
			return true;
		}

		// A6: the sim thread parks in the lockstep wait while a peer is silent, so the admission plane
		// has to be serviced from inside it - the silent peer may be waiting on the very answer only
		// that pump can send, which is what left every clean leave unacknowledged.
		// Four peers over a host-star loopback, each reporting N changing sound readings a frame, so the
		// relay's own byte counters say what the compact form costs. The same frames priced the way
		// version 13 spelled them out give the factor. The first frame and the steady ones are measured
		// apart, because only the first spells its keys out.
		bool TestFourPeerObservationRelayBytes(std::string* error) {
			for (const size_t observationsPerFrame: {size_t{64}, size_t{256}, size_t{512}}) {
				const uint16_t port = static_cast<uint16_t>(43040 + observationsPerFrame % 16);
				const uint64_t sessionId = 0x7000000000000040ULL + observationsPerFrame;
				LoopbackTransport hostT, clientAT, clientBT, clientCT;
				if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) ||
				    !clientBT.Connect("loopback", port, error) || !clientCT.Connect("loopback", port, error)) {
					return false;
				}
				auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
					NetLockstepConfig c;
					c.sessionId = sessionId;
					c.startFrame = 0;
					c.inputDelayFrames = 0;
					c.timeoutMs = 4000;
					c.localPeerId = local;
					c.peerCount = 4;
					c.remoteTransportPeerIds = std::move(transports);
					c.relayToOtherPeers = relay;
					c.frameLane = NetTransportLane::ControlReliable;
					c.scenario = "LockstepSelfTest";
					c.ownershipPolicy = "unique-id-split";
					c.roundId = 0x1400000000000001ULL + observationsPerFrame;
					return c;
				};
				NetLockstepCoordinator host, clientA, clientB, clientC;
				NetLockstepConfig clientCfg = cfg(2, {{1, 1}}, false);
				clientCfg.roundId = 0;
				if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}, {4, 3}}, true), error)) {
					return false;
				}
				clientCfg.localPeerId = 2;
				if (!clientA.Start(clientAT, clientCfg, error)) {
					return false;
				}
				clientCfg.localPeerId = 3;
				if (!clientB.Start(clientBT, clientCfg, error)) {
					return false;
				}
				clientCfg.localPeerId = 4;
				if (!clientC.Start(clientCT, clientCfg, error)) {
					return false;
				}
				NetLockstepCoordinator* peers[4] = {&host, &clientA, &clientB, &clientC};
				LoopbackTransport* transports[4] = {&hostT, &clientAT, &clientBT, &clientCT};
				auto drive = [&](const std::function<bool()>& done) {
					for (uint64_t now = 0; now <= 20000; now += 5) {
						for (NetLockstepCoordinator* peer: peers) {
							peer->Tick(now);
						}
						if (done()) {
							return true;
						}
						for (LoopbackTransport* transport: transports) {
							transport->AdvanceTimeMs(5);
						}
					}
					return false;
				};
				if (!drive([&] { return host.IsRunning() && clientA.IsRunning() && clientB.IsRunning() && clientC.IsRunning(); })) {
					*error = "four-peer lockstep did not reach Running";
					return false;
				}

				std::map<uint8_t, std::map<uint64_t, std::vector<NetSoundObservation>>> committed;
				size_t readyPerPeer[4] = {0, 0, 0, 0};
				auto collect = [&](size_t index) {
					NetLockstepReadyFrame ready;
					while (peers[index]->PopReadyFrame(ready)) {
						std::vector<NetSoundObservation> all = ready.localObservations;
						all.insert(all.end(), ready.remoteObservations.begin(), ready.remoteObservations.end());
						std::stable_sort(all.begin(), all.end(), [](const NetSoundObservation& a, const NetSoundObservation& b) {
							return std::tie(a.senderPeerId, a.objectUID, a.ordinal) < std::tie(b.senderPeerId, b.objectUID, b.ordinal);
						});
						committed[static_cast<uint8_t>(index)][ready.frame] = std::move(all);
						++readyPerPeer[index];
					}
				};
				// Version 13 priced the same frames at forty-four bytes an observation, on top of a packet
				// that is otherwise byte for byte the same one this build sends.
				size_t legacyBytesPerFrame = 0;
				const size_t frameCount = 12;
				const auto runFrames = [&](uint64_t from, uint64_t to) {
					for (uint64_t f = from; f < to; ++f) {
						for (uint8_t peer = 1; peer <= 4; ++peer) {
							std::vector<NetSoundObservation> observations = MakeObservationSet(peer, observationsPerFrame, 400 + peer, static_cast<float>(f) / 16.0F);
							if (peer == 1 && legacyBytesPerFrame == 0) {
								NetLockstepFrame priced;
								priced.senderPeerId = peer;
								priced.targetFrame = f;
								priced.roundId = 0x1400000000000001ULL + observationsPerFrame;
								priced.frames = {MakeFrame(100 + static_cast<int64_t>(f), f + 1)};
								priced.observations = observations;
								// Less the binding sequence of an empty block, which version 13 did not have.
								legacyBytesPerFrame = FrameBytesWithoutObservations(priced) - 1 + 44 * observationsPerFrame;
							}
							if (!peers[peer - 1]->QueueLocalInput(f, {MakeFrame(100 * peer + static_cast<int64_t>(f), f + 1)}, {}, error, observations)) {
								return false;
							}
						}
					}
					if (!drive([&] {
							for (size_t i = 0; i < 4; ++i) {
								collect(i);
							}
							return readyPerPeer[0] >= to && readyPerPeer[1] >= to && readyPerPeer[2] >= to && readyPerPeer[3] >= to;
						})) {
						*error = "four-peer lockstep did not produce every ready frame: " + std::to_string(readyPerPeer[0]) + "," + std::to_string(readyPerPeer[1]) +
						         "," + std::to_string(readyPerPeer[2]) + "," + std::to_string(readyPerPeer[3]);
						return false;
					}
					return true;
				};
				const uint64_t startBytes = host.GetStats().relayBytesSent;
				const uint32_t startPackets = host.GetStats().relayPacketsSent;
				if (!runFrames(0, 1)) {
					return false;
				}
				const uint64_t firstUseBytes = host.GetStats().relayBytesSent - startBytes;
				const uint32_t firstUsePackets = host.GetStats().relayPacketsSent - startPackets;
				if (!runFrames(1, frameCount)) {
					return false;
				}
				const uint64_t steadyBytes = host.GetStats().relayBytesSent - startBytes - firstUseBytes;
				const uint32_t steadyPackets = host.GetStats().relayPacketsSent - startPackets - firstUsePackets;

				// Every peer commits the identical table, which is the whole point of the wire form.
				for (uint64_t f = 0; f < frameCount; ++f) {
					for (uint8_t peer = 1; peer < 4; ++peer) {
						if (committed[peer][f] != committed[0][f]) {
							*error = "four-peer observation tables differ at frame " + std::to_string(f) + " on peer " + std::to_string(peer + 1);
							return false;
						}
					}
					if (committed[0][f].size() != observationsPerFrame * 4) {
						*error = "four-peer frame " + std::to_string(f) + " committed " + std::to_string(committed[0][f].size()) +
						         " observations, expected " + std::to_string(observationsPerFrame * 4);
						return false;
					}
				}
				const NetLockstepStats& stats = host.GetStats();
				if (stats.relayObservationOverflows != 0 || stats.unresolvedObservationPackets != 0 ||
				    stats.observationsCarried != 0 || stats.observationsDropped != 0) {
					*error = "four-peer relay reported an observation fault";
					return false;
				}
				if (firstUsePackets == 0 || steadyPackets == 0) {
					*error = "four-peer relay forwarded nothing to measure";
					return false;
				}
				const double firstUsePerPacket = static_cast<double>(firstUseBytes) / firstUsePackets;
				const double steadyPerPacket = static_cast<double>(steadyBytes) / steadyPackets;
				const double legacy = static_cast<double>(legacyBytesPerFrame);
				std::cout << "[net-lockstep-selftest] PASS four_peer_observation_relay n=" << observationsPerFrame
				          << " relayed_bytes_per_frame first_use=" << firstUsePerPacket << " steady=" << steadyPerPacket
				          << " (was " << legacy << ") factor first_use=" << legacy / firstUsePerPacket << " steady=" << legacy / steadyPerPacket
				          << " relay_bytes_sent=" << stats.relayBytesSent << " largest_relay_packet_bytes=" << stats.largestRelayPacketBytes << std::endl;
				if (legacy / steadyPerPacket < 6.0) {
					*error = "the compact observation form saved less than six times in the steady state at n=" + std::to_string(observationsPerFrame);
					return false;
				}
			}
			return true;
		}

		// More changed readings in one frame than its byte budget holds: the sender keeps the rest for the
		// next frame instead of refusing the frame, and both peers commit the same table either way.
		bool TestObservationOverflowCarry(std::string* error) {
			LoopbackTransport hostTransport, clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, 0x7000000000000050ULL, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, 0x7000000000000050ULL, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = 0x1400000000000050ULL;
			hostConfig.timeoutMs = 4000;
			clientConfig.timeoutMs = 4000;
			if (!StartCoordinatorPair(43060, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error, 8000)) {
				return false;
			}
			// Every key is new every frame, so the block is nothing but spelled-out keys and the budget bites.
			uint64_t nextObject = 30000000;
			const auto flood = [&](size_t count, uint64_t tick) {
				std::vector<NetSoundObservation> observations;
				for (size_t i = 0; i < count; ++i) {
					observations.push_back(MakeObservation(1, nextObject++, tick, 0xF0E1D2C3B4A59687ULL + nextObject, 1 + i % 4, 0.125F));
				}
				return observations;
			};
			// Two frames of new sounds nobody can hold, then quiet ones for the backlog to drain into.
			const size_t burst = 2048;
			const size_t frameCount = 10;
			std::vector<NetSoundObservation> sent;
			for (uint64_t f = 0; f < frameCount; ++f) {
				std::vector<NetSoundObservation> observations = f < 2 ? flood(burst, 700 + f) : std::vector<NetSoundObservation>{};
				sent.insert(sent.end(), observations.begin(), observations.end());
				if (!host.QueueLocalInput(f, {MakeFrame(100 + static_cast<int64_t>(f), f + 1)}, {}, error, observations) ||
				    !client.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error)) {
					return false;
				}
			}
			std::vector<NetSoundObservation> hostSeen, clientSeen;
			size_t hostReady = 0, clientReady = 0;
			auto collect = [&](NetLockstepCoordinator& coordinator, std::vector<NetSoundObservation>& into, size_t& count) {
				NetLockstepReadyFrame ready;
				while (coordinator.PopReadyFrame(ready)) {
					into.insert(into.end(), ready.localObservations.begin(), ready.localObservations.end());
					into.insert(into.end(), ready.remoteObservations.begin(), ready.remoteObservations.end());
					++count;
				}
			};
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] {
					collect(host, hostSeen, hostReady);
					collect(client, clientSeen, clientReady);
					return hostReady >= frameCount && clientReady >= frameCount;
				}, error, 16000)) {
				return false;
			}
			if (host.GetStats().observationsCarried == 0) {
				*error = "a frame of nothing but new keys did not carry anything over";
				return false;
			}
			if (host.GetStats().observationsDropped != 0) {
				*error = "a burst the quiet frames could drain still dropped readings";
				return false;
			}
			// The two peers saw the same readings in the same order, and the burst arrived whole.
			if (hostSeen != clientSeen) {
				*error = "the carried observations reached the two peers differently";
				return false;
			}
			if (hostSeen != sent) {
				*error = "the carried observations did not all arrive in their sampled order: " +
				         std::to_string(hostSeen.size()) + " of " + std::to_string(sent.size());
				return false;
			}
			const uint64_t carriedInBurst = host.GetStats().observationsCarried;

			// Sustained: more new sounds every frame than the wire can ever carry. The held set stays
			// bounded, the oldest readings go, and the frames themselves keep flowing.
			for (uint64_t f = frameCount; f < frameCount + 8; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100 + static_cast<int64_t>(f), f + 1)}, {}, error, flood(NetLockstepCodec::c_MaxObservationsPerPacket, 800 + f)) ||
				    !client.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error)) {
					return false;
				}
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] {
					collect(host, hostSeen, hostReady);
					collect(client, clientSeen, clientReady);
					return hostReady >= frameCount + 8 && clientReady >= frameCount + 8;
				}, error, 16000)) {
				return false;
			}
			if (host.GetStats().observationsDropped == 0) {
				*error = "the held observation set was not bounded under a sustained flood";
				return false;
			}
			// Every dropped reading comes back exactly once, in the order it was sampled, so its sampler
			// can forget it was ever sent and offer it again.
			const std::vector<NetSoundObservation> handedBack = host.TakeDroppedObservations();
			if (handedBack.size() != host.GetStats().observationsDropped) {
				*error = "the dropped readings were not all handed back: " + std::to_string(handedBack.size()) +
				         " of " + std::to_string(host.GetStats().observationsDropped);
				return false;
			}
			std::set<NetSoundObservationKey> committedKeys;
			for (const NetSoundObservation& observation: hostSeen) {
				committedKeys.insert(KeyOfObservation(observation));
			}
			for (const NetSoundObservation& observation: handedBack) {
				if (committedKeys.contains(KeyOfObservation(observation))) {
					*error = "a reading was both committed and handed back as dropped";
					return false;
				}
			}
			if (!host.TakeDroppedObservations().empty()) {
				*error = "a dropped reading was handed back twice";
				return false;
			}
			if (hostSeen != clientSeen) {
				*error = "a bounded held set left the two peers with different observations";
				return false;
			}
			if (host.IsFailed() || client.IsFailed()) {
				*error = "driving past the observation cap failed the round";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS observation_overflow_carry burst=" << burst << " carried=" << carriedInBurst
			          << " delivered=" << sent.size() << "/" << sent.size() << " sustained_dropped=" << host.GetStats().observationsDropped << std::endl;
			return true;
		}

		// A frame from another lockstep round is the round's business, not the codec's: it decodes like
		// any other, its sender still counts as heard from, and the round drops it.
		bool TestStaleRoundFrameStillCountsAsTraffic(std::string* error) {
			LoopbackTransport hostTransport, clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, 0x7000000000000070ULL, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, 0x7000000000000070ULL, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = 0x1400000000000070ULL;
			hostConfig.timeoutMs = 4000;
			clientConfig.timeoutMs = 4000;
			if (!StartCoordinatorPair(43070, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error, 4000)) {
				return false;
			}
			const NetLockstepPeerStats before = host.GetStats().peers.at(2);
			const uint32_t staleBefore = host.GetStats().staleRoundPackets;

			// A frame of the round before this one, with observations whose slots this host was never given.
			NetSoundObservationDictionary strangerEncoder;
			NetLockstepFrame stale;
			stale.senderPeerId = 2;
			stale.targetFrame = 4;
			stale.roundId = hostConfig.roundId - 1;
			stale.frames = {MakeFrame(300, 1)};
			stale.observations = MakeObservationSet(2, 6, 55, 0.0F);
			std::vector<uint8_t> bytes;
			size_t encoded = 0;
			if (!NetLockstepCodec::Encode({stale}, bytes, nullptr, &strangerEncoder, &encoded) || encoded != stale.observations.size()) {
				*error = "the stale-round frame did not encode";
				return false;
			}
			if (!clientTransport.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			for (uint64_t now = 5000; now <= 5100; now += 5) {
				host.Tick(now);
				hostTransport.AdvanceTimeMs(5);
				clientTransport.AdvanceTimeMs(5);
			}
			const NetLockstepPeerStats after = host.GetStats().peers.at(2);
			if (after.staleRoundPackets != before.staleRoundPackets + 1 || host.GetStats().staleRoundPackets != staleBefore + 1) {
				*error = "a stale-round frame was not counted against its sender";
				return false;
			}
			if (after.framePacketsReceived != before.framePacketsReceived + 1) {
				*error = "a stale-round frame did not count as a frame packet from its sender";
				return false;
			}
			if (after.lastHeardMs <= before.lastHeardMs) {
				*error = "a stale-round frame did not prove its sender is still there";
				return false;
			}
			if (host.GetStats().unresolvedObservationPackets != 0 || host.IsFailed()) {
				*error = "a stale-round frame disturbed the round";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS stale_round_frame_counts_as_traffic last_heard=" << before.lastHeardMs << "->" << after.lastHeardMs
			          << " stale=" << after.staleRoundPackets << " unresolved=0" << std::endl;
			return true;
		}

		// Two frames a relay host cannot read the observations of, for opposite reasons: one from a
		// transport that is not in the round at all, one from a peer that is. Only the second is a fault.
		bool TestObservationFaultsAreToldApart(std::string* error) {
			const uint16_t port = 43080;
			const uint64_t sessionId = 0x7000000000000080ULL;
			LoopbackTransport hostT, clientAT, clientBT, strangerT;
			if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) ||
			    !clientBT.Connect("loopback", port, error) || !strangerT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.startFrame = 0;
				c.inputDelayFrames = 0;
				c.timeoutMs = 4000;
				c.localPeerId = local;
				c.peerCount = 3;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.frameLane = NetTransportLane::ControlReliable;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				c.roundId = 0x1400000000000080ULL;
				return c;
			};
			NetLockstepCoordinator host, clientA, clientB;
			NetLockstepConfig clientCfg = cfg(2, {{1, 1}}, false);
			clientCfg.roundId = 0;
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error)) {
				return false;
			}
			clientCfg.localPeerId = 2;
			if (!clientA.Start(clientAT, clientCfg, error)) {
				return false;
			}
			clientCfg.localPeerId = 3;
			if (!clientB.Start(clientBT, clientCfg, error)) {
				return false;
			}
			NetLockstepCoordinator* peers[3] = {&host, &clientA, &clientB};
			LoopbackTransport* transports[4] = {&hostT, &clientAT, &clientBT, &strangerT};
			auto drive = [&](uint64_t from, uint64_t to) {
				for (uint64_t now = from; now <= to; now += 5) {
					for (NetLockstepCoordinator* peer: peers) {
						peer->Tick(now);
					}
					for (LoopbackTransport* transport: transports) {
						transport->AdvanceTimeMs(5);
					}
				}
			};
			drive(0, 1000);
			if (!host.IsRunning()) {
				*error = "the fault-classification host did not reach Running";
				return false;
			}

			// A frame whose observations refer to slots this host was never given.
			NetSoundObservationDictionary strangerEncoder;
			std::vector<uint8_t> binding, reference;
			size_t encoded = 0;
			if (!NetLockstepCodec::Encode({MakeObservationFrame(2, 0x1400000000000080ULL, MakeObservationSet(2, 4, 31, 0.0F))}, binding, nullptr, &strangerEncoder, &encoded) ||
			    !NetLockstepCodec::Encode({MakeObservationFrame(3, 0x1400000000000080ULL, MakeObservationSet(2, 4, 31, 0.5F))}, reference, nullptr, &strangerEncoder, &encoded)) {
				*error = "the unreadable frames did not encode";
				return false;
			}

			// From a transport with no seat in the round: admission traffic, not a fault of ours.
			const uint32_t admissionBefore = host.GetStats().ignoredAdmissionFaults;
			if (!strangerT.Send(1, NetTransportLane::ControlReliable, reference, error)) {
				return false;
			}
			drive(1005, 1200);
			if (host.GetStats().ignoredAdmissionFaults != admissionBefore + 1 || host.GetStats().unresolvedObservationPackets != 0) {
				*error = "an unknown transport's unreadable frame was counted as a fault: admission=" +
				         std::to_string(host.GetStats().ignoredAdmissionFaults) + " unresolved=" + std::to_string(host.GetStats().unresolvedObservationPackets);
				return false;
			}

			// From a peer of the round, the same shape means a binding it can never be told again.
			if (!clientAT.Send(1, NetTransportLane::ControlReliable, reference, error)) {
				return false;
			}
			drive(1205, 1400);
			if (host.GetStats().unresolvedObservationPackets != 1) {
				*error = "a round member's unreadable frame was not counted as a fault";
				return false;
			}
			if (host.IsFailed() || !host.IsRunning()) {
				*error = "an unreadable observation block ended the round";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS observation_faults_told_apart admission=" << host.GetStats().ignoredAdmissionFaults
			          << " unresolved=" << host.GetStats().unresolvedObservationPackets << std::endl;
			return true;
		}

		// A frame the round is going to discard must not disturb the live table of the peer that sent it,
		// whether its sender's encoder is fresh or ahead. The reviewer's stale_round_shape probe.
		bool TestStaleRoundFrameLeavesTheLiveTable(std::string* error) {
			for (int shape = 0; shape < 2; ++shape) {
				LoopbackTransport hostTransport, clientTransport;
				NetLockstepCoordinator host, client;
				const uint64_t sessionId = 0x7000000000000090ULL + static_cast<uint64_t>(shape);
				NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::ControlReliable);
				NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::ControlReliable);
				hostConfig.roundId = 0x1400000000000090ULL;
				hostConfig.timeoutMs = 8000;
				clientConfig.timeoutMs = 8000;
				if (!StartCoordinatorPair(static_cast<uint16_t>(43090 + shape), hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
					return false;
				}
				// One clock for the whole probe, so the peer's last-heard stamp can be read as it moves.
				uint64_t clock = 0;
				size_t hostReady = 0;
				std::vector<NetSoundObservation> hostSaw;
				auto drive = [&](const std::function<bool()>& done) {
					for (uint64_t step = 0; step < 2000; ++step) {
						// The clock moves before every tick, so a peer's last-heard stamp reads as it moves.
						hostTransport.AdvanceTimeMs(5);
						clientTransport.AdvanceTimeMs(5);
						clock += 5;
						host.Tick(clock);
						client.Tick(clock);
						NetLockstepReadyFrame ready;
						while (host.PopReadyFrame(ready)) {
							hostSaw = ready.remoteObservations;
							++hostReady;
						}
						if (done()) {
							return true;
						}
					}
					return false;
				};
				if (!drive([&] { return host.IsRunning() && client.IsRunning(); })) {
					*error = "the straggler probe did not reach Running";
					return false;
				}
				// Real traffic first, so the host's table for peer 2 is not empty when the straggler lands.
				const auto play = [&](uint64_t from, uint64_t to) {
					for (uint64_t f = from; f < to; ++f) {
						if (!host.QueueLocalInput(f, {MakeFrame(100 + static_cast<int64_t>(f), f + 1)}, {}, error) ||
						    !client.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error, MakeObservationSet(2, 12, 61, static_cast<float>(f) / 8.0F))) {
							return false;
						}
					}
					return drive([&] { return hostReady >= to; });
				};
				if (!play(0, 3)) {
					if (error->empty()) { *error = "the straggler probe's opening frames did not commit"; }
					return false;
				}
				const NetLockstepPeerStats before = host.GetStats().peers.at(2);
				const uint32_t unresolvedBefore = host.GetStats().unresolvedObservationPackets;

				// A frame of the previous round. Shape 0 comes from a fresh encoder, which is the zero count
				// that would reset a live table; shape 1 comes from one that is ahead of the host's.
				NetSoundObservationDictionary strangerEncoder;
				size_t encoded = 0;
				if (shape == 1) {
					for (int warm = 0; warm < 3; ++warm) {
						std::vector<uint8_t> scratch;
						if (!NetLockstepCodec::Encode({MakeObservationFrame(90 + warm, hostConfig.roundId - 1, MakeObservationSet(2, 9, 300 + warm, 0.0F))}, scratch, nullptr, &strangerEncoder, &encoded)) {
							*error = "the straggler's warm-up did not encode";
							return false;
						}
					}
				}
				NetLockstepFrame stale;
				stale.senderPeerId = 2;
				stale.targetFrame = 3;
				stale.roundId = hostConfig.roundId - 1;
				stale.frames = {MakeFrame(999, 1)};
				stale.observations = MakeObservationSet(2, 6, 400, 0.25F);
				std::vector<uint8_t> bytes;
				if (!NetLockstepCodec::Encode({stale}, bytes, nullptr, &strangerEncoder, &encoded) || encoded != stale.observations.size()) {
					*error = "the stale-round frame did not encode";
					return false;
				}
				if (!clientTransport.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
					return false;
				}
				if (!drive([&] { return host.GetStats().peers.at(2).staleRoundPackets > before.staleRoundPackets; })) {
					*error = "shape " + std::to_string(shape) + ": the straggler never reached the round";
					return false;
				}
				const NetLockstepPeerStats after = host.GetStats().peers.at(2);
				if (after.staleRoundPackets != before.staleRoundPackets + 1 || after.framePacketsReceived != before.framePacketsReceived + 1) {
					*error = "shape " + std::to_string(shape) + ": the straggler was not counted against its sender";
					return false;
				}
				if (after.lastHeardMs <= before.lastHeardMs) {
					*error = "shape " + std::to_string(shape) + ": the straggler did not prove its sender is still there (" +
					         std::to_string(before.lastHeardMs) + " -> " + std::to_string(after.lastHeardMs) + ")";
					return false;
				}
				if (host.GetStats().unresolvedObservationPackets != unresolvedBefore) {
					*error = "shape " + std::to_string(shape) + ": a discarded straggler was counted as a hole in its sender's stream";
					return false;
				}

				// The live table has to have survived: the client's next frames are mostly slot references.
				if (!play(3, 5)) {
					*error = "shape " + std::to_string(shape) + ": the peer was muted after the straggler";
					return false;
				}
				if (hostReady < 5 || hostSaw.size() != 12) {
					*error = "shape " + std::to_string(shape) + ": the host committed " + std::to_string(hostReady) +
					         " frames and " + std::to_string(hostSaw.size()) + " observations after the straggler";
					return false;
				}
				if (host.GetStats().unresolvedObservationPackets != unresolvedBefore || host.IsFailed()) {
					*error = "shape " + std::to_string(shape) + ": the round did not survive the straggler";
					return false;
				}
				std::cout << "[net-lockstep-selftest] PASS stale_round_leaves_the_live_table shape=" << shape
				          << " stale=" << after.staleRoundPackets << " unresolved=" << host.GetStats().unresolvedObservationPackets
				          << " last_heard=" << before.lastHeardMs << "->" << after.lastHeardMs
				          << " frames=" << hostReady << " observations=" << hostSaw.size() << std::endl;
			}
			return true;
		}

		// A resync restarts the host while the other peer is still producing frames of the old round. Those
		// are ordinary stragglers, not holes. The reviewer's resync_straggler_counter probe.
		bool TestResyncStragglersAreNotHoles(std::string* error) {
			LoopbackTransport hostTransport, clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, 0x70000000000000A0ULL, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, 0x70000000000000A0ULL, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = 0x14000000000000A0ULL;
			hostConfig.timeoutMs = 20000;
			clientConfig.timeoutMs = 20000;
			if (!StartCoordinatorPair(43092, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			uint64_t clock = 0;
			size_t hostReady = 0;
			auto drive = [&](const std::function<bool()>& done) {
				for (uint64_t step = 0; step < 3000; ++step) {
					// The clock moves before every tick, so a peer's last-heard stamp reads as it moves.
					hostTransport.AdvanceTimeMs(5);
					clientTransport.AdvanceTimeMs(5);
					clock += 5;
					host.Tick(clock);
					client.Tick(clock);
					NetLockstepReadyFrame ready;
					while (host.PopReadyFrame(ready)) {
						++hostReady;
					}
					if (done()) {
						return true;
					}
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); })) {
				*error = "the resync probe did not reach Running";
				return false;
			}
			for (uint64_t f = 0; f < 3; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100 + static_cast<int64_t>(f), f + 1)}, {}, error) ||
				    !client.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error, MakeObservationSet(2, 10, 71, static_cast<float>(f) / 8.0F))) {
					return false;
				}
			}
			if (!drive([&] { return hostReady >= 3; })) {
				*error = "the resync probe's opening frames did not commit";
				return false;
			}

			// The host restarts into a new round; the client has not noticed yet and keeps sending.
			NetLockstepConfig resyncConfig = hostConfig;
			resyncConfig.roundId = hostConfig.roundId + 1;
			resyncConfig.remoteTransportPeerId = 1;
			if (!host.Start(hostTransport, resyncConfig, error)) {
				return false;
			}
			for (uint64_t f = 3; f < 6; ++f) {
				if (!client.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error, MakeObservationSet(2, 10, 71, static_cast<float>(f) / 8.0F))) {
					return false;
				}
			}
			drive([&] { return host.GetStats().staleRoundPackets >= 3; });
			const NetLockstepStats& afterStragglers = host.GetStats();
			if (afterStragglers.unresolvedObservationPackets != 0) {
				*error = "a resync's in-flight frames were counted as holes: " + std::to_string(afterStragglers.unresolvedObservationPackets);
				return false;
			}
			if (afterStragglers.staleRoundPackets != 3 || afterStragglers.peers.at(2).staleRoundPackets != 3) {
				*error = "a resync's in-flight frames were not counted as stragglers: " + std::to_string(afterStragglers.staleRoundPackets);
				return false;
			}
			if (afterStragglers.peers.at(2).lastHeardMs == 0) {
				*error = "a resync's in-flight frames did not prove their sender is still there";
				return false;
			}
			const uint64_t stragglerStamp = afterStragglers.peers.at(2).lastHeardMs;

			// The client catches up; the round re-forms and runs.
			NetLockstepConfig clientResync = clientConfig;
			clientResync.remoteTransportPeerId = 1;
			if (!client.Start(clientTransport, clientResync, error)) {
				return false;
			}
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); })) {
				*error = "the round did not re-form after the resync";
				return false;
			}
			hostReady = 0;
			for (uint64_t f = 0; f < 4; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(300 + static_cast<int64_t>(f), f + 1)}, {}, error) ||
				    !client.QueueLocalInput(f, {MakeFrame(400 + static_cast<int64_t>(f), f + 1)}, {}, error, MakeObservationSet(2, 10, 81, static_cast<float>(f) / 8.0F))) {
					return false;
				}
			}
			if (!drive([&] { return hostReady >= 4; })) {
				*error = "the recovered round did not commit its frames";
				return false;
			}
			if (host.GetStats().unresolvedObservationPackets != 0) {
				*error = "the recovered round reported a hole";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS resync_stragglers_are_not_holes stale=" << host.GetStats().staleRoundPackets
			          << " unresolved=0 last_heard=" << stragglerStamp << " recovered_frames=" << hostReady << std::endl;
			return true;
		}

		// A block that fails part way through must leave the table exactly as it was, or the sender's next
		// perfectly good packet reads as a hole. The reviewer's partial_bind_then_refuse probe.
		bool TestRefusedBlockLeavesNoBindings(std::string* error) {
			NetSoundObservationDictionary sender;
			NetSoundObservationTables receiver;
			std::vector<uint8_t> first;
			size_t encoded = 0;
			if (!NetLockstepCodec::Encode({MakeObservationFrame(10, 0, MakeObservationSet(2, 6, 91, 0.0F))}, first, nullptr, &sender, &encoded) || encoded != 6) {
				*error = "the truncation probe's frame did not encode";
				return false;
			}
			// Cut the block in half and re-stamp the payload length, so the packet is well formed up to the
			// point where it runs out.
			std::vector<uint8_t> truncated(first.begin(), first.begin() + static_cast<std::ptrdiff_t>(first.size() - 40));
			for (int i = 0; i < 4; ++i) {
				truncated[12 + i] = static_cast<uint8_t>((truncated.size() - NetLockstepCodec::c_HeaderBytes) >> (i * 8));
			}
			const NetLockstepDecodeResult refused = NetLockstepCodec::Decode(truncated, ControllerFrame::c_Version, &receiver);
			if (refused.ok || refused.error.code != NetLockstepErrorCode::TruncatedPayload) {
				*error = "a truncated observation block was not refused: " + std::string(NetLockstepCodec::ErrorCodeName(refused.error.code));
				return false;
			}
			if (receiver.Exactly(2).BindingCount() != 0) {
				*error = "a refused block left " + std::to_string(receiver.Exactly(2).BindingCount()) + " bindings behind";
				return false;
			}
			// The same frame, whole, still reads, and so does the one after it.
			const NetLockstepDecodeResult whole = NetLockstepCodec::Decode(first, ControllerFrame::c_Version, &receiver);
			if (!whole.ok) {
				*error = "the whole frame did not decode after its truncated copy: " + std::string(NetLockstepCodec::ErrorCodeName(whole.error.code));
				return false;
			}
			std::vector<uint8_t> next;
			const std::vector<NetSoundObservation> repeats = MakeObservationSet(2, 6, 91, 0.5F);
			if (!NetLockstepCodec::Encode({MakeObservationFrame(11, 0, repeats)}, next, nullptr, &sender, &encoded)) {
				*error = "the frame after the truncation did not encode";
				return false;
			}
			const NetLockstepDecodeResult following = NetLockstepCodec::Decode(next, ControllerFrame::c_Version, &receiver);
			const NetLockstepFrame* followingFrame = following.ok ? std::get_if<NetLockstepFrame>(&following.packet.payload) : nullptr;
			if (!followingFrame || followingFrame->observations != repeats) {
				*error = "the sender's next packet was refused after a truncated one: " + std::string(NetLockstepCodec::ErrorCodeName(following.error.code));
				return false;
			}
			// Staging must not change which bindings land or what a slot means while the block reads: a block
			// that spells one slot out twice binds twice, and the reference between the two reads the first key.
			NetSoundObservationTables ordered;
			std::vector<uint8_t> crafted;
			if (!NetLockstepCodec::Encode({MakeObservationFrame(12, 0, {})}, crafted)) {
				*error = "the ordering probe's frame did not encode";
				return false;
			}
			crafted.resize(crafted.size() - 3); // The empty observation block: a zero binding sequence and a zero count.
			const auto appendVar = [&crafted](uint64_t value) {
				while (value >= 0x80U) {
					crafted.push_back(static_cast<uint8_t>(value) | 0x80U);
					value >>= 7;
				}
				crafted.push_back(static_cast<uint8_t>(value));
			};
			const auto appendReading = [&crafted](float value) {
				uint32_t bits = 0;
				std::memcpy(&bits, &value, sizeof(bits));
				for (int i = 0; i < 4; ++i) {
					crafted.push_back(static_cast<uint8_t>(bits >> (i * 8)));
				}
			};
			const NetSoundObservationKey firstKey{4001, 7, 0x9E3779B97F4A7C15ULL, 1, 2};
			const NetSoundObservationKey secondKey{4002, 9, 0xC2B2AE3D27D4EB4FULL, 3, 4};
			const auto appendSlotZeroKey = [&](const NetSoundObservationKey& key) {
				appendVar(1); // Slot 0, spelled out.
				crafted.push_back(0x1F);
				appendVar(key.objectUID);
				appendVar(key.tick);
				appendVar(key.phase);
				appendVar(key.occurrence);
				appendVar(key.ordinal);
			};
			appendVar(0); // This sender has spelled nothing out before the block.
			crafted.push_back(3);
			crafted.push_back(0);
			appendSlotZeroKey(firstKey);
			appendReading(0.25F);
			appendVar(0); // Slot 0 by reference, between the two keys it is bound to.
			appendReading(0.5F);
			appendSlotZeroKey(secondKey);
			appendReading(0.75F);
			for (int i = 0; i < 4; ++i) {
				crafted[12 + i] = static_cast<uint8_t>((crafted.size() - NetLockstepCodec::c_HeaderBytes) >> (i * 8));
			}
			const NetLockstepDecodeResult twice = NetLockstepCodec::Decode(crafted, ControllerFrame::c_Version, &ordered);
			const NetLockstepFrame* twiceFrame = twice.ok ? std::get_if<NetLockstepFrame>(&twice.packet.payload) : nullptr;
			if (!twiceFrame || twiceFrame->observations.size() != 3) {
				*error = "a block spelling one slot twice did not decode: " + std::string(NetLockstepCodec::ErrorCodeName(twice.error.code));
				return false;
			}
			NetSoundObservationKey settled;
			if (KeyOfObservation(twiceFrame->observations[1]) != firstKey || KeyOfObservation(twiceFrame->observations[2]) != secondKey ||
			    ordered.Exactly(2).BindingCount() != 2 || !ordered.Exactly(2).Resolve(0, settled) || settled != secondKey) {
				*error = "staged bindings did not land in the order the block spelled them";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS refused_block_leaves_no_bindings bindings_after_refusal=0 next_packet=ok"
			          << " one_slot_twice=2_bindings mid_block_reference=first_key" << std::endl;
			return true;
		}

		bool TestSessionPumpRunsWhileTheRoundWaits(std::string* error) {
			const uint16_t port = 43050;
			const uint64_t sessionId = 0x7000000000000050ULL;
			LoopbackTransport hostT, clientT;
			if (!hostT.StartHost(port, error) || !clientT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.timeoutMs = 200;
				c.localPeerId = local;
				c.peerCount = 2;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			NetLockstepCoordinator host, client;
			if (!host.Start(hostT, cfg(1, {{2, 1}}, true), error) || !client.Start(clientT, cfg(2, {{1, 1}}, false), error)) {
				return false;
			}
			for (uint64_t now = 0; now <= 2000; now += 5) {
				host.Tick(now);
				client.Tick(now);
				if (host.IsRunning() && client.IsRunning()) {
					break;
				}
				hostT.AdvanceTimeMs(5);
				clientT.AdvanceTimeMs(5);
			}
			if (!host.IsRunning()) {
				*error = "the pump fixture did not reach Running";
				return false;
			}
			// The client never sends frame 0, so the host waits for it and times out.
			if (!host.QueueLocalInput(0, {MakeFrame(100, 1)}, {}, error)) {
				return false;
			}
			uint32_t pumps = 0;
			ScenarioRunner::SetLockstepCoordinator(&host);
			ScenarioRunner::SetSessionPump([&pumps] { ++pumps; });
			NetLockstepReadyFrame ready;
			std::string waitError;
			const bool got = ScenarioRunner::WaitForLockstepControllerFrame(0, ready, &waitError);
			ScenarioRunner::SetSessionPump(nullptr);
			ScenarioRunner::SetLockstepCoordinator(nullptr);
			if (got) {
				*error = "the wait returned a frame the peer never sent";
				return false;
			}
			if (pumps == 0) {
				*error = "the admission plane was never serviced while the round waited";
				return false;
			}
			// Paced, not spun: a wait of a few hundred ms must not run the plane's clock away.
			if (pumps > 200) {
				*error = "the wait pumped the admission plane " + std::to_string(pumps) + " times, unpaced";
				return false;
			}
			return true;
		}

		// The relay host is the only route between its clients, so its own last tick is not the round's
		// end: a client one input-delay behind still needs the forwards the host is holding. Quitting
		// there took the last frames off every client that was waiting - the 4-peer lane's 179 of 180.
		bool TestRelayHostFinishesWhatItOwes(std::string* error) {
			StarFixture fx;
			const uint32_t timeoutMs = 20000; // Long enough that the unreachable bound cannot end this.
			std::vector<uint64_t> hostReady, bReady;
			if (!StartRelayRefusal(fx, 43016, 0x7000000000000016ULL, timeoutMs, hostReady, error)) {
				return false;
			}
			const uint64_t refusedFrom = fx.now;
			fx.DriveProducing(true, [&] { return fx.host.GetStats().relaySendFailures > 0; }, refusedFrom + 400);
			if (fx.host.GetStats().relaySendFailures == 0) {
				*error = "the refusal never reached the relay: " + fx.host.BuildReportJson();
				return false;
			}
			if (!fx.host.HasPendingRelayWork()) {
				*error = "a refused forward left the host owing nothing: " + fx.host.BuildReportJson();
				return false;
			}
			// Completing the round does not discharge the debt: the frames are still undelivered.
			fx.host.Complete("host reached its tick cap");
			if (!fx.host.HasPendingRelayWork()) {
				*error = "Complete() dropped the forwards the host still owed";
				return false;
			}
			fx.Collect(fx.clientB, bReady);
			const size_t behind = bReady.size();
			// The link comes back and the host is given the chance to hand over what it holds.
			fx.hostT.SetFaultConfig({});
			for (const uint64_t until = fx.now + 2000; fx.now <= until && fx.host.HasPendingRelayWork(); fx.now += 5) {
				fx.host.Tick(fx.now);
				fx.clientB.Tick(fx.now);
				fx.hostT.AdvanceTimeMs(5);
				fx.clientBT.AdvanceTimeMs(5);
			}
			if (fx.host.HasPendingRelayWork()) {
				*error = "the held forwards never drained: " + fx.host.BuildReportJson();
				return false;
			}
			for (const uint64_t until = fx.now + 500; fx.now <= until; fx.now += 5) {
				fx.clientB.Tick(fx.now);
				fx.clientBT.AdvanceTimeMs(5);
				fx.Collect(fx.clientB, bReady);
			}
			if (bReady.size() <= behind) {
				*error = "the peer that was owed forwards never received them";
				return false;
			}
			// The accounting the next lane run reads: bytes, not just packet counts.
			const NetLockstepStats& stats = fx.host.GetStats();
			if (stats.relayBytesSent == 0 || stats.largestRelayPacketBytes == 0 ||
			    stats.peers.at(3).relayBytesSent == 0 || stats.relayBacklogBytes != 0) {
				*error = "the relay byte accounting is missing: " + fx.host.BuildReportJson();
				return false;
			}
			const std::string report = fx.host.BuildReportJson();
			if (report.find("\"relay_bytes_sent\":") == std::string::npos ||
			    report.find("\"largest_relay_packet_bytes\":") == std::string::npos ||
			    report.find("\"relay_backlog_bytes\":") == std::string::npos) {
				*error = "the relay byte counters are missing from the report: " + report;
				return false;
			}
			return true;
		}
		// Four peers behind a 200ms link whose send queue is metered, which is the lobby lane's shape.
		// A relay host that outruns its own socket must hold the forwards and let the round wait: the
		// peers behind that queue are healthy, and taking their seats for it ended the match at frame
		// 151 with three "unreachable" leaves the retained run shows were our congestion, not theirs.
		// One peer behind a link that stops draining while every other link stays healthy - the review's
		// failing case. The round must lose that seat and go on, never the other way round: whether the
		// bound that takes it is the hold clock or the backlog cap, only one seat may pay for it.
		bool RunOneDeadLinkRound(bool healEarly, bool healLate, std::string* error, std::string* verdict, bool neverBreak = false) {
			const uint16_t port = static_cast<uint16_t>(healEarly ? 43030 : 43040);
			const uint64_t sessionId = 0x7000000000000040ULL + (healEarly ? 0 : 1);
			const uint32_t timeoutMs = 4000;
			LoopbackTransport hostT, clientT[3];
			LoopbackTransportConfig lagged;
			lagged.latencyMs = 200;
			LoopbackTransportConfig dead = lagged;
			dead.sendBufferBytes = 384;
			dead.drainBytesPerSecond = 0;
			dead.meterOnlyPeer = 1; // transport id 1 is lockstep peer 2; every other link stays healthy.
			hostT.SetFaultConfig(lagged);
			if (!hostT.StartHost(port, error)) {
				return false;
			}
			for (LoopbackTransport& client : clientT) {
				client.SetFaultConfig(lagged);
				if (!client.Connect("loopback", port, error)) {
					return false;
				}
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.timeoutMs = timeoutMs;
				c.localPeerId = local;
				c.peerCount = 4;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			NetLockstepCoordinator host, client[3];
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}, {4, 3}}, true), error) ||
			    !client[0].Start(clientT[0], cfg(2, {{1, 1}}, false), error) ||
			    !client[1].Start(clientT[1], cfg(3, {{1, 1}}, false), error) ||
			    !client[2].Start(clientT[2], cfg(4, {{1, 1}}, false), error)) {
				return false;
			}
			uint64_t now = 0;
			uint64_t produced[4] = {0, 0, 0, 0};
			std::string queueError;
			auto step = [&](const std::function<bool()>& done, uint64_t untilMs) {
				for (; now <= untilMs; now += 5) {
					std::string ignored;
					if (host.IsRunning() && produced[0] <= host.GetStats().nextFrame + 4 &&
					    host.QueueLocalInput(produced[0], {MakeFrame(100, produced[0] + 1)}, {}, &queueError)) {
						++produced[0];
					}
					for (int i = 0; i < 3; ++i) {
						if (client[i].IsRunning() && produced[i + 1] <= client[i].GetStats().nextFrame + 4 &&
						    client[i].QueueLocalInput(produced[i + 1], {MakeFrame(200 + i * 100, produced[i + 1] + 1)}, {}, &queueError)) {
							++produced[i + 1];
						}
					}
					host.Tick(now);
					for (int i = 0; i < 3; ++i) {
						client[i].Tick(now);
					}
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					for (LoopbackTransport& transport : clientT) {
						transport.AdvanceTimeMs(5);
					}
				}
				return false;
			};
			if (!step([&] { return host.IsRunning() && client[0].IsRunning() && client[1].IsRunning() && client[2].IsRunning(); }, 4000)) {
				*error = "the one-dead-link fixture did not reach Running";
				return false;
			}
			LoopbackTransportConfig healed = lagged;
			healed.sendBufferBytes = 1 << 20;
			healed.drainBytesPerSecond = 1 << 20;
			const uint64_t brokeAt = now;
			if (!neverBreak) {
				hostT.SetFaultConfig(dead);
			}
			if (neverBreak) {
				// Control: nothing is wrong with any link, so the round has only itself to blame.
			} else if (healEarly) {
				// Inside every bound: the stream has not lost a forward yet, so the link coming back is
				// all it takes.
				if (!step([&] { return host.GetStats().relayCongestedRefusals > 0; }, now + timeoutMs)) {
					*error = "the dead link never refused a forward: " + host.BuildReportJson();
					return false;
				}
				hostT.SetFaultConfig(healed);
			} else {
				if (!step([&] { return !host.GetPeerLeaveFrames().empty(); }, now + 2 * timeoutMs)) {
					*error = "the dead link cost nobody a seat: " + host.BuildReportJson();
					return false;
				}
				const uint64_t leftAt = now;
				const NetLockstepStats& s = host.GetStats();
				// The hold clock starts at the first refusal, not at the break, so what has to hold is the
				// property the bound exists for: the seat goes before the round's own grace could end it.
				if (leftAt - brokeAt >= timeoutMs || s.longestCongestionHoldMs >= timeoutMs) {
					*error = "the seat outlasted the round's grace (" + std::to_string(leftAt - brokeAt) + "ms, hold " +
					         std::to_string(s.longestCongestionHoldMs) + "ms): " + host.BuildReportJson();
					return false;
				}
				if (host.GetPeerLeaveFrames().size() != 1 || host.GetPeerLeaveFrames().find(2) == host.GetPeerLeaveFrames().end()) {
					*error = "a healthy peer paid for the dead link: " + host.BuildReportJson();
					return false;
				}
				const auto peerIt = s.peers.find(2);
				if (peerIt == s.peers.end() || (peerIt->second.relayBacklogOverflows == 0 && peerIt->second.longestCongestionHoldMs == 0)) {
					*error = "the leave is not attributed to peer 2's own queue: " + host.BuildReportJson();
					return false;
				}
				// The seat went, so the connection goes with it, in the same tick that took the seat.
				if (hostT.IsPeerConnected(1) || s.connectionsClosedOnEviction != 1) {
					*error = "the evicted seat kept its connection (closed=" + std::to_string(s.connectionsClosedOnEviction) +
					         "): " + host.BuildReportJson();
					return false;
				}
				// The close is the notice: the evicted peer stops there and then, rather than spending
				// its own grace sending into a round that is no longer listening.
				if (!client[0].IsFailed() || client[0].GetStats().timeoutReason.find("PeerDisconnected") == std::string::npos) {
					*error = "the evicted peer did not stop when its seat was taken: " + client[0].BuildReportJson();
					return false;
				}
				if (healLate) {
					hostT.SetFaultConfig(healed);
				}
			}
			if (!healEarly && !neverBreak && host.GetPeerLeaveFrames().count(2) != 0) {
				host.ResolveHeldSeat(2, NetLockstepHoldResolution::Expired, now);
			}
			// The round is what has to survive: the remaining peers keep committing frames past the seat
			// the dead link cost, which is the whole point of taking only that one.
			const uint64_t resumeFrom = host.GetStats().nextFrame;
			// A round that has shed a seat runs to the same length as the control: shedding one is not
			// allowed to cost the survivors anything.
			const uint64_t target = 45;
			if (!step([&] { return host.GetStats().nextFrame >= resumeFrom + target; }, now + 6 * timeoutMs)) {
				*error = "the round stopped advancing; last queue refusal [" + queueError + "] produced=" +
				         std::to_string(produced[0]) + "/" + std::to_string(produced[1]) + "/" + std::to_string(produced[2]) + "/" +
				         std::to_string(produced[3]) + " " + host.BuildReportJson() +
				         " |c2 " + client[0].BuildReportJson() + " |c3 " + client[1].BuildReportJson() +
				         " |c4 " + client[2].BuildReportJson();
				return false;
			}
			if (host.IsFailed()) {
				*error = "the round failed after the dead link was resolved: " + host.BuildReportJson();
				return false;
			}
			const size_t expectedLeaves = (healEarly || neverBreak) ? 0 : 1;
			if (host.GetPeerLeaveFrames().size() != expectedLeaves) {
				*error = "the round shed " + std::to_string(host.GetPeerLeaveFrames().size()) + " seats, expected " +
				         std::to_string(expectedLeaves) + ": " + host.BuildReportJson();
				return false;
			}
			for (int i = 0; i < 3; ++i) {
				if (i == 0 && !healEarly && !neverBreak) {
					continue; // The peer whose link died is allowed to have stopped.
				}
				if (client[i].GetStats().nextFrame + 5 < resumeFrom + target) {
					*error = "a healthy client stalled at frame " + std::to_string(client[i].GetStats().nextFrame) +
					         ": " + client[i].BuildReportJson();
					return false;
				}
			}
			// The survivors run the same round: a seat going has to land on the same frame for both.
			if (client[1].GetStats().nextFrame != client[2].GetStats().nextFrame) {
				*error = "the survivors ended on different frames (" + std::to_string(client[1].GetStats().nextFrame) + " vs " +
				         std::to_string(client[2].GetStats().nextFrame) + "): " + client[1].BuildReportJson() + " |c4 " + client[2].BuildReportJson();
				return false;
			}
			const NetLockstepStats& s = host.GetStats();
			*verdict = " peers_left=" + std::to_string(host.GetPeerLeaveFrames().size()) +
			           " frames=" + std::to_string(s.nextFrame) +
			           " refusals=" + std::to_string(s.relayCongestedRefusals) +
			           " holds=" + std::to_string(s.relayCongestionHolds) +
			           " longest_hold_ms=" + std::to_string(s.longestCongestionHoldMs) +
			           " overflows=" + std::to_string(s.relayBacklogOverflows) +
			           " ignored_stops=" + std::to_string(s.stopsFromLeftPeers) +
			           " closed_on_eviction=" + std::to_string(s.connectionsClosedOnEviction) +
			           " client_frames=" + std::to_string(client[0].GetStats().nextFrame) + "/" +
			           std::to_string(client[1].GetStats().nextFrame) + "/" + std::to_string(client[2].GetStats().nextFrame);
			return true;
		}

		// Control: nothing is broken, so nothing may be shed and the round must keep committing.
		bool TestFourPeerRoundRunsToLength(std::string* error) {
			std::string verdict;
			if (!RunOneDeadLinkRound(false, false, error, &verdict, true)) {
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS four_peer_round_runs_to_length" << verdict << std::endl;
			return true;
		}

		bool TestDeadLinkLosesOnlyItsOwnSeat(std::string* error) {
			std::string verdict;
			if (!RunOneDeadLinkRound(false, true, error, &verdict)) {
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS dead_link_loses_only_its_own_seat" << verdict << std::endl;
			return true;
		}

		bool TestDeadLinkHealedInTimeKeepsEverySeat(std::string* error) {
			std::string verdict;
			if (!RunOneDeadLinkRound(true, false, error, &verdict)) {
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS dead_link_healed_in_time_keeps_every_seat" << verdict << std::endl;
			return true;
		}

		bool TestCongestedRelayHoldsEveryPeer(std::string* error) {
			const uint16_t port = 43020;
			const uint64_t sessionId = 0x7000000000000020ULL;
			// A 200ms link needs a round trip per frame, so the round's own grace has to be seconds, not
			// the 400ms the quick fixtures use.
			const uint32_t timeoutMs = 4000; // The hold bound is half of this.
			LoopbackTransport hostT, clientT[3];
			LoopbackTransportConfig lagged;
			lagged.latencyMs = 200;
			LoopbackTransportConfig metered = lagged;
			// A couple of forwards fill it and nothing meaningful drains: the socket the host has to
			// keep three remotes fed through is simply gone.
			metered.sendBufferBytes = 384;
			metered.drainBytesPerSecond = 64;
			hostT.SetFaultConfig(lagged);
			if (!hostT.StartHost(port, error)) {
				return false;
			}
			for (LoopbackTransport& client : clientT) {
				client.SetFaultConfig(lagged);
				if (!client.Connect("loopback", port, error)) {
					return false;
				}
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.timeoutMs = timeoutMs;
				c.localPeerId = local;
				c.peerCount = 4;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			NetLockstepCoordinator host, client[3];
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}, {4, 3}}, true), error) ||
			    !client[0].Start(clientT[0], cfg(2, {{1, 1}}, false), error) ||
			    !client[1].Start(clientT[1], cfg(3, {{1, 1}}, false), error) ||
			    !client[2].Start(clientT[2], cfg(4, {{1, 1}}, false), error)) {
				return false;
			}
			uint64_t now = 0;
			uint64_t produced[4] = {0, 0, 0, 0};
			auto step = [&](const std::function<bool()>& done, uint64_t untilMs) {
				for (; now <= untilMs; now += 5) {
					std::string ignored;
					if (host.IsRunning() && produced[0] <= host.GetStats().nextFrame + 4 &&
					    host.QueueLocalInput(produced[0], {MakeFrame(100, produced[0] + 1)}, {}, &ignored)) {
						++produced[0];
					}
					for (int i = 0; i < 3; ++i) {
						if (client[i].IsRunning() && produced[i + 1] <= client[i].GetStats().nextFrame + 4 &&
						    client[i].QueueLocalInput(produced[i + 1], {MakeFrame(200 + i * 100, produced[i + 1] + 1)}, {}, &ignored)) {
							++produced[i + 1];
						}
					}
					host.Tick(now);
					for (int i = 0; i < 3; ++i) {
						client[i].Tick(now);
					}
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					for (LoopbackTransport& transport : clientT) {
						transport.AdvanceTimeMs(5);
					}
				}
				return false;
			};
			if (!step([&] { return host.IsRunning() && client[0].IsRunning() && client[1].IsRunning() && client[2].IsRunning(); }, 4000)) {
				*error = "the four-peer congestion fixture did not reach Running";
				return false;
			}
			// The socket fills once the round is up, as it did at frame 151: the handshake is not what
			// this fixture is about.
			hostT.SetFaultConfig(metered);
			if (!step([&] { return host.GetStats().relayCongestedRefusals > 0; }, now + timeoutMs)) {
				*error = "the metered send queue never refused a forward: " + host.BuildReportJson();
				return false;
			}
			const uint64_t congestedFrom = now;
			// Past the bound that used to condemn a peer, and inside the round's own grace.
			step([&] { return false; }, congestedFrom + timeoutMs / 2 + 200);
			if (host.GetStats().relayCongestionHolds == 0) {
				*error = "a congestion episode outlasting the bound was not recorded as a hold: " + host.BuildReportJson();
				return false;
			}
			if (!host.GetPeerLeaveFrames().empty()) {
				*error = "the host took a seat off a peer because its own queue was full: " + host.BuildReportJson();
				return false;
			}
			if (host.GetStats().peersDroppedSilent != 0) {
				*error = "a peer behind our own queue was adjudicated silent: " + host.BuildReportJson();
				return false;
			}
			// The link comes back: the held forwards go out and the round is still whole.
			LoopbackTransportConfig drained = lagged;
			drained.sendBufferBytes = 1 << 20;
			drained.drainBytesPerSecond = 1 << 20;
			hostT.SetFaultConfig(drained);
			if (!step([&] { return !host.HasPendingRelayWork(); }, now + 2 * timeoutMs)) {
				*error = "the held forwards never drained once the queue freed up: " + host.BuildReportJson();
				return false;
			}
			if (!host.GetPeerLeaveFrames().empty() || host.IsFailed()) {
				*error = "the round did not survive the congestion episode: " + host.BuildReportJson();
				return false;
			}
			const NetLockstepStats& stats = host.GetStats();
			if (stats.relayResends == 0 || stats.relayBacklogBytes != 0) {
				*error = "the backlog did not actually replay: " + host.BuildReportJson();
				return false;
			}
			// And the round goes on: surviving the episode means committing frames after it, not just
			// keeping four names on the roster.
			const uint64_t resumedFrom = stats.nextFrame;
			if (!step([&] { return stats.nextFrame >= resumedFrom + 3; }, now + 2 * timeoutMs)) {
				*error = "the round kept its peers but never advanced again: " + host.BuildReportJson();
				return false;
			}
			const std::string report = host.BuildReportJson();
			if (report.find("\"relay_congested_refusals\":") == std::string::npos ||
			    report.find("\"relay_congestion_holds\":") == std::string::npos ||
			    report.find("\"relay_backlog_overflows\":") == std::string::npos) {
				*error = "the congestion counters are missing from the report: " + report;
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS congested_relay_holds_every_peer refusals=" << stats.relayCongestedRefusals
			          << " holds=" << stats.relayCongestionHolds << " resends=" << stats.relayResends
			          << " dropped_silent=" << stats.peersDroppedSilent << " peers_left=" << host.GetPeerLeaveFrames().size()
			          << " frames=" << stats.nextFrame << std::endl;
			return true;
		}
		// These fixtures drive a handshake that a build without the fixes answers with another start, so
		// they stop on a start budget as well as a clock: a fixture must fail, never fill the machine.
		constexpr uint32_t c_RoundStartBudget = 200;

		uint32_t StartsSent(std::initializer_list<const NetLockstepCoordinator*> peers) {
			uint32_t sent = 0;
			for (const NetLockstepCoordinator* peer: peers) {
				sent += peer->GetStats().startPacketsSent;
			}
			return sent;
		}

		// Every start a running peer receives reads as a repeat, so an answer that is itself a start
		// used to answer the answer: one stray start bounced between two peers forever.
		bool TestCoordinatorRepeatedStartsDoNotAmplify(std::string* error) {
			const uint16_t port = 43102;
			const uint64_t sessionId = 0x70000000000000A2ULL;
			const uint64_t roundId = 0x9A5E0000000000A2ULL;
			LoopbackTransport hostTransport, clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = roundId;
			hostConfig.timeoutMs = 20000;
			clientConfig.timeoutMs = 20000;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			uint64_t now = 0;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 5, now += 5) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					if (StartsSent({&host, &client}) > c_RoundStartBudget) {
						return false;
					}
					hostTransport.AdvanceTimeMs(5);
					clientTransport.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); }, 1000)) {
				*error = "the round never started";
				return false;
			}
			const uint32_t hostSentBefore = host.GetStats().startPacketsSent;
			const uint32_t clientSentBefore = client.GetStats().startPacketsSent;
			NetLockstepStart repeat;
			repeat.sessionId = sessionId;
			repeat.startFrame = 0;
			repeat.inputDelayFrames = 0;
			repeat.controllerFrameVersion = ControllerFrame::c_Version;
			repeat.controllerFrameEncodedSize = static_cast<uint16_t>(ControllerFrame::c_EncodedSize);
			repeat.localPeerId = 2;
			repeat.peerCount = 2;
			repeat.scenario = "LockstepSelfTest";
			repeat.ownershipPolicy = "unique-id-split";
			repeat.roundId = roundId;
			std::vector<uint8_t> bytes;
			if (!EncodePacket({repeat}, bytes, error) || !clientTransport.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			drive([&] { return false; }, 1000);
			const uint32_t hostSent = host.GetStats().startPacketsSent - hostSentBefore;
			const uint32_t clientSent = client.GetStats().startPacketsSent - clientSentBefore;
			// One second is four retransmit intervals; a start each way per interval is the whole budget.
			if (hostSent > 8 || clientSent > 8) {
				*error = "one repeated start amplified into " + std::to_string(hostSent) + " host and " +
				         std::to_string(clientSent) + " client starts in a second";
				return false;
			}
			if (host.IsFailed() || client.IsFailed() || !host.IsRunning() || !client.IsRunning()) {
				*error = "a repeated start disturbed the running round";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS repeated_starts_do_not_amplify host=" << hostSent << " client=" << clientSent << std::endl;
			return true;
		}
		// A peer repeating its start is missing one and cannot say whose, so the host owes it the whole
		// round: its own start and every remote start it has taken. Client B starts late here, so the
		// relayed start of client A drains into the void and only a re-relay can reach it; client C
		// starts later still, so the host is answering while it is itself waiting for a start.
		bool TestCoordinatorRepeatedStartCarriesTheRound(std::string* error) {
			const uint16_t port = 43100;
			const uint64_t sessionId = 0x70000000000000A0ULL;
			const uint64_t roundId = 0x9A5E0000000000A0ULL;
			LoopbackTransport hostT, clientAT, clientBT, clientCT;
			if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) ||
			    !clientBT.Connect("loopback", port, error) || !clientCT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.startFrame = 0;
				c.inputDelayFrames = 0;
				c.timeoutMs = 20000;
				c.localPeerId = local;
				c.peerCount = 4;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.frameLane = NetTransportLane::ControlReliable;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				c.roundId = relay ? roundId : 0;
				return c;
			};
			NetLockstepCoordinator host, clientA, clientB, clientC;
			uint64_t now = 0;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 5, now += 5) {
					host.Tick(now);
					clientA.Tick(now);
					clientB.Tick(now);
					clientC.Tick(now);
					if (done()) {
						return true;
					}
					if (StartsSent({&host, &clientA, &clientB, &clientC}) > c_RoundStartBudget) {
						return false;
					}
					hostT.AdvanceTimeMs(5);
					clientAT.AdvanceTimeMs(5);
					clientBT.AdvanceTimeMs(5);
					clientCT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}, {4, 3}}, true), error) || !clientA.Start(clientAT, cfg(2, {{1, 1}}, false), error)) {
				return false;
			}
			if (!drive([&] { return host.GetStats().startPacketsReceived >= 1; }, 500)) {
				*error = "the host never took client A's start";
				return false;
			}
			// Client B is between rounds: the host's start and the relayed start of A drain into the void.
			clientBT.AdvanceTimeMs(10);
			if (clientBT.PollEvents().empty()) {
				*error = "the relayed start never reached client B's transport";
				return false;
			}
			if (!clientB.Start(clientBT, cfg(3, {{1, 1}}, false), error)) {
				return false;
			}
			// B repeats while the host still waits for C, so the answer has to come from a waiting host.
			if (!drive([&] { return clientB.GetStats().startRetransmits >= 2; }, 1500)) {
				*error = "client B never repeated its start";
				return false;
			}
			if (host.IsRunning()) {
				*error = "the host ran before client C started, so this fixture proves nothing";
				return false;
			}
			if (!clientC.Start(clientCT, cfg(4, {{1, 1}}, false), error)) {
				return false;
			}
			if (!drive([&] { return host.IsRunning() && clientA.IsRunning() && clientB.IsRunning() && clientC.IsRunning(); }, 3000)) {
				*error = "a repeated start was not answered with the round: " + clientB.BuildReportJson();
				return false;
			}
			if (host.GetStats().startsRelayedOnRepeat == 0) {
				*error = "client B reached Running without the host re-relaying a start";
				return false;
			}
			for (uint64_t producedFrame = 0; producedFrame < 3; ++producedFrame) {
				if (!host.QueueLocalInput(producedFrame, {MakeFrame(100 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, {}, error) ||
				    !clientA.QueueLocalInput(producedFrame, {MakeFrame(200 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, {}, error) ||
				    !clientB.QueueLocalInput(producedFrame, {MakeFrame(300 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, {}, error) ||
				    !clientC.QueueLocalInput(producedFrame, {MakeFrame(400 + static_cast<int64_t>(producedFrame), producedFrame + 1)}, {}, error)) {
					return false;
				}
			}
			std::vector<uint64_t> hostReady, aReady, bReady, cReady;
			if (!drive([&] {
					DrainReady(host, hostReady);
					DrainReady(clientA, aReady);
					DrainReady(clientB, bReady);
					DrainReady(clientC, cReady);
					return hostReady.size() == 3 && aReady.size() == 3 && bReady.size() == 3 && cReady.size() == 3;
				}, 2000)) {
				*error = "the re-formed four-peer round did not commit";
				return false;
			}
			// Another remote's start is not this peer's round to move: relayed, so it never owns the
			// transport it arrives on, and a round tag it disagrees with makes it a straggler.
			const uint64_t staleBefore = clientB.GetStats().staleRoundPackets;
			NetLockstepStart relayedStraggler;
			relayedStraggler.sessionId = sessionId;
			relayedStraggler.startFrame = 0;
			relayedStraggler.inputDelayFrames = 0;
			relayedStraggler.controllerFrameVersion = ControllerFrame::c_Version;
			relayedStraggler.controllerFrameEncodedSize = static_cast<uint16_t>(ControllerFrame::c_EncodedSize);
			relayedStraggler.localPeerId = 2;
			relayedStraggler.peerCount = 4;
			relayedStraggler.scenario = "LockstepSelfTest";
			relayedStraggler.ownershipPolicy = "unique-id-split";
			relayedStraggler.roundId = roundId + 1;
			std::vector<uint8_t> bytes;
			if (!EncodePacket({relayedStraggler}, bytes, error) || !hostT.Send(2, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			if (!drive([&] { return clientB.GetStats().staleRoundPackets == staleBefore + 1; }, 500)) {
				*error = "a relayed start from another round was not ignored";
				return false;
			}
			if (clientB.GetRoundId() != roundId || clientB.IsFailed()) {
				*error = "a relayed start moved client B's round";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS repeated_start_carries_the_round relayed=" << host.GetStats().startsRelayedOnRepeat << std::endl;
			return true;
		}
		// A peer that has played this round already had every start; a new start from it means it has
		// LEFT the round, and answering with ours would hand it a round it is not in.
		bool TestCoordinatorRestartedPeerIsNotHandedTheOldRound(std::string* error) {
			const uint16_t port = 43101;
			const uint64_t sessionId = 0x70000000000000A1ULL;
			const uint64_t roundOne = 0x9A5E0000000000A1ULL;
			const uint64_t roundTwo = roundOne + 0x100ULL;
			LoopbackTransport hostTransport, clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = roundOne;
			hostConfig.timeoutMs = 20000;
			clientConfig.timeoutMs = 20000;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			uint64_t now = 0;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 5, now += 5) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					if (StartsSent({&host, &client}) > c_RoundStartBudget) {
						return false;
					}
					hostTransport.AdvanceTimeMs(5);
					clientTransport.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); }, 1000)) {
				*error = "the first round never started";
				return false;
			}
			if (!host.QueueLocalInput(0, {MakeFrame(100, 1)}, {}, error) || !client.QueueLocalInput(0, {MakeFrame(200, 1)}, {}, error)) {
				return false;
			}
			if (!drive([&] { return host.GetStats().framesAccepted == 1 && client.GetStats().framesAccepted == 1; }, 1000)) {
				*error = "the first round did not commit a frame";
				return false;
			}
			// The client restarts first; the host is still running the round the client just left.
			NetLockstepConfig clientRestart = clientConfig;
			clientRestart.roundId = 0;
			clientRestart.remoteTransportPeerId = 1;
			if (!client.Start(clientTransport, clientRestart, error)) {
				return false;
			}
			drive([&] { return false; }, 1000);
			if (client.GetRoundId() != 0 || client.IsRunning()) {
				*error = "the restarted client was handed the round it had just left (round " + std::to_string(client.GetRoundId()) + ")";
				return false;
			}
			if (host.GetStats().startAnswersSuppressed == 0) {
				*error = "the host answered a peer that had already played this round";
				return false;
			}
			// The host follows, and the round re-forms on the host's new tag.
			NetLockstepConfig hostRestart = hostConfig;
			hostRestart.roundId = roundTwo;
			hostRestart.remoteTransportPeerId = 1;
			if (!host.Start(hostTransport, hostRestart, error)) {
				return false;
			}
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); }, 4000)) {
				*error = "the round never re-formed after the host restarted: host=" + host.BuildReportJson() + " client=" + client.BuildReportJson();
				return false;
			}
			if (client.GetRoundId() != roundTwo) {
				*error = "the client did not converge on the host's new round";
				return false;
			}
			if (!host.QueueLocalInput(0, {MakeFrame(101, 1)}, {}, error) || !client.QueueLocalInput(0, {MakeFrame(201, 1)}, {}, error)) {
				return false;
			}
			if (!drive([&] { return host.GetStats().framesAccepted == 1 && client.GetStats().framesAccepted == 1; }, 1000)) {
				*error = "the re-formed round did not commit a frame";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS restarted_peer_is_not_handed_the_old_round suppressed" << std::endl;
			return true;
		}
		// A client that restarted before the host did adopts the round the host is still finishing;
		// when the host starts its own, the client has to follow it rather than refuse it forever.
		bool TestCoordinatorClientFollowsTheHostsNewRound(std::string* error) {
			const uint16_t port = 43103;
			const uint64_t sessionId = 0x70000000000000A3ULL;
			const uint64_t roundOne = 0x9A5E0000000000A3ULL;
			const uint64_t roundTwo = roundOne + 0x100ULL;
			NetLockstepFrame resent{2, 0, {MakeFrame(200, 1)}, {}, roundTwo, {}};
			LoopbackTransport hostTransport;
			RecoveryTapTransport clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = roundOne;
			hostConfig.timeoutMs = 20000;
			clientConfig.timeoutMs = 20000;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			uint64_t now = 0;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 5, now += 5) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					if (StartsSent({&host, &client}) > c_RoundStartBudget) {
						return false;
					}
					hostTransport.AdvanceTimeMs(5);
					clientTransport.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); }, 1000)) {
				*error = "the first round never started";
				return false;
			}
			// The client restarts before it has played, so nothing tells the host it has left; it takes
			// the host's answer and lands in the round the host is about to leave.
			NetLockstepConfig clientRestart = clientConfig;
			clientRestart.roundId = 0;
			clientRestart.remoteTransportPeerId = 1;
			if (!client.Start(clientTransport, clientRestart, error)) {
				return false;
			}
			if (!drive([&] { return client.GetRoundId() == roundOne; }, 2000)) {
				*error = "the restarted client never took the old round, so this fixture proves nothing";
				return false;
			}
			// It plays into that round too: this frame goes out under a tag the host's next round refuses.
			if (!client.QueueLocalInput(0, {MakeFrame(200, 1)}, {}, error)) {
				return false;
			}
			drive([&] { return false; }, 100);
			// A start from another round for another start frame stays a straggler, never a new round.
			const uint32_t staleBefore = client.GetStats().staleRoundPackets;
			NetLockstepStart straggler;
			straggler.sessionId = sessionId;
			straggler.startFrame = 7;
			straggler.inputDelayFrames = 0;
			straggler.controllerFrameVersion = ControllerFrame::c_Version;
			straggler.controllerFrameEncodedSize = static_cast<uint16_t>(ControllerFrame::c_EncodedSize);
			straggler.localPeerId = 1;
			straggler.peerCount = 2;
			straggler.scenario = "LockstepSelfTest";
			straggler.ownershipPolicy = "unique-id-split";
			straggler.roundId = roundOne - 1;
			std::vector<uint8_t> bytes;
			if (!EncodePacket({straggler}, bytes, error) || !hostTransport.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			if (!drive([&] { return client.GetStats().staleRoundPackets == staleBefore + 1; }, 500)) {
				*error = "a start for another start frame was not ignored";
				return false;
			}
			if (client.IsFailed() || client.GetRoundId() != roundOne) {
				*error = "a straggling start moved or failed the client's round";
				return false;
			}
			// The host starts its own round; the client's has committed nothing, so it follows.
			NetLockstepConfig hostRestart = hostConfig;
			hostRestart.roundId = roundTwo;
			hostRestart.remoteTransportPeerId = 1;
			if (!host.Start(hostTransport, hostRestart, error)) {
				return false;
			}
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); }, 4000)) {
				*error = "the client never followed the host onto its new round: host=" + host.BuildReportJson() + " client=" + client.BuildReportJson();
				return false;
			}
			if (client.GetRoundId() != roundTwo || client.GetStats().roundReadoptions != 1) {
				*error = "the client is running on round " + std::to_string(client.GetRoundId()) + " after " +
				         std::to_string(client.GetStats().roundReadoptions) + " readoptions";
				return false;
			}
			// The client's frame was produced for this round under the old tag; the round commits it.
			if (!host.QueueLocalInput(0, {MakeFrame(100, 1)}, {}, error)) {
				return false;
			}
			if (!drive([&] { return host.GetStats().framesAccepted == 1 && client.GetStats().framesAccepted == 1; }, 1000)) {
				*error = "the followed round did not commit the client's own production: host=" + host.BuildReportJson();
				return false;
			}
			// Once the round has committed, no start moves it: a running match keeps its round.
			const uint32_t staleAfterCommit = client.GetStats().staleRoundPackets;
			NetLockstepStart afterCommit = straggler;
			afterCommit.startFrame = 0;
			afterCommit.roundId = roundOne;
			if (!EncodePacket({afterCommit}, bytes, error) || !hostTransport.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			if (!drive([&] { return client.GetStats().staleRoundPackets == staleAfterCommit + 1; }, 500)) {
				*error = "a start from another round reached a committed round";
				return false;
			}
			if (client.GetRoundId() != roundTwo || client.GetStats().roundReadoptions != 1 || client.IsFailed()) {
				*error = "a committed round was moved by another round's start";
				return false;
			}
			if (!CheckRecoveredInput(clientTransport, resent, sessionId, true, error)) return false;
			std::cout << "[net-lockstep-selftest] PASS client_follows_the_hosts_new_round readoptions=" << client.GetStats().roundReadoptions << std::endl;
			return true;
		}
		// ---- round-start-review: adversarial cases -------------------------------------------------
		// Independent review of stage2/round-start-fixes. These are written to hold what the branch
		// claims; a RED line here is a finding against the branch, not against the base.
		constexpr uint32_t c_ReviewStartBudget = 3000;

		// ReadoptRound leaves the round without clearing what the round said about who is GONE.
		// Start() clears m_PeerLeaveFrames/m_LeftSeatsHeld; ReadoptRound keeps them, so the one peer
		// that FOLLOWS a new round carries the old round's leave map into it and alone stops requiring
		// the returned peer's frames - which is exactly the peer the rejoin resync exists to bring back.
		bool TestReviewReadoptClearsTheLeftPeer(std::string* error) {
			const uint16_t port = 43110;
			const uint64_t sessionId = 0x70000000000000B0ULL;
			const uint64_t roundOne = 0x9A5E0000000000B0ULL;
			const uint64_t roundTwo = roundOne + 0x100ULL;
			LoopbackTransport hostT, clientAT, clientBT;
			if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) ||
			    !clientBT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay, uint64_t round) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.startFrame = 0;
				c.inputDelayFrames = 0;
				c.timeoutMs = 60000;
				c.localPeerId = local;
				c.peerCount = 3;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.frameLane = NetTransportLane::ControlReliable;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				c.roundId = round;
				return c;
			};
			NetLockstepCoordinator host, clientA, clientB;
			uint64_t now = 0;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 5, now += 5) {
					host.Tick(now);
					clientA.Tick(now);
					clientB.Tick(now);
					if (done()) {
						return true;
					}
					if (StartsSent({&host, &clientA, &clientB}) > c_ReviewStartBudget) {
						return false;
					}
					hostT.AdvanceTimeMs(5);
					clientAT.AdvanceTimeMs(5);
					clientBT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true, roundOne), error) ||
			    !clientA.Start(clientAT, cfg(2, {{1, 1}}, false, 0), error) ||
			    !clientB.Start(clientBT, cfg(3, {{1, 1}}, false, 0), error)) {
				return false;
			}
			if (!drive([&] { return host.IsRunning() && clientA.IsRunning() && clientB.IsRunning(); }, 2000)) {
				*error = "the first round never started";
				return false;
			}
			// Peer 3 drops; the host adjudicates and announces the leave at frame 0, which is the notice
			// ApplyPeerLeave relays. Client A has committed nothing, so it stays eligible to follow.
			NetLockstepStop leave;
			leave.senderPeerId = 3;
			leave.reason = NetLockstepStopReason::PeerLeft;
			leave.frame = 0;
			leave.message = "connection lost";
			std::vector<uint8_t> bytes;
			if (!EncodePacket({leave}, bytes, error) || !hostT.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			drive([&] { return false; }, 100);
			if (clientA.GetStats().framesAccepted != 0 || !clientA.IsRunning()) {
				*error = "client A is not in the state this fixture needs (accepted=" +
				         std::to_string(clientA.GetStats().framesAccepted) + ")";
				return false;
			}
			// Peer 3 comes back: the host resyncs onto a new round and peer 3 restarts into it. Client A
			// never restarts - it follows.
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true, roundTwo), error) ||
			    !clientB.Start(clientBT, cfg(3, {{1, 1}}, false, 0), error)) {
				return false;
			}
			if (!drive([&] { return host.IsRunning() && clientA.IsRunning() && clientB.IsRunning(); }, 6000)) {
				*error = "the round never re-formed: A=" + clientA.BuildReportJson();
				return false;
			}
			if (clientA.GetStats().roundReadoptions != 1 || clientA.GetRoundId() != roundTwo) {
				*error = "client A did not follow the host onto the new round (readoptions=" +
				         std::to_string(clientA.GetStats().roundReadoptions) + ")";
				return false;
			}
			// In the NEW round peer 3 is present and required by everyone. Only the host and A produce.
			if (!host.QueueLocalInput(0, {MakeFrame(100, 1)}, {}, error) ||
			    !clientA.QueueLocalInput(0, {MakeFrame(200, 1)}, {}, error)) {
				return false;
			}
			drive([&] { return false; }, 500);
			if (clientA.GetStats().framesAccepted != 0 || host.GetStats().framesAccepted != 0) {
				*error = "the followed round committed a different frame set on the follower: A accepted=" +
				         std::to_string(clientA.GetStats().framesAccepted) + " host accepted=" +
				         std::to_string(host.GetStats().framesAccepted) +
				         " - A carried the old round's leave record for peer 3 through ReadoptRound";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS review_readopt_clears_the_left_peer" << std::endl;
			return true;
		}

		// A stop the round deferred belongs to the round that deferred it. Start() drops it; ReadoptRound
		// keeps it, so a peer that follows the host into the next match stops that one at its first tick.
		bool TestReviewReadoptClearsTheDeferredStop(std::string* error) {
			const uint16_t port = 43113;
			const uint64_t sessionId = 0x70000000000000B3ULL;
			const uint64_t roundOne = 0x9A5E0000000000B3ULL;
			const uint64_t roundTwo = roundOne + 0x100ULL;
			LoopbackTransport hostTransport, clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = roundOne;
			hostConfig.timeoutMs = 60000;
			clientConfig.timeoutMs = 60000;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			client.DeferStopsToTickBoundary();
			uint64_t now = 0;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 5, now += 5) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					if (StartsSent({&host, &client}) > c_ReviewStartBudget) {
						return false;
					}
					hostTransport.AdvanceTimeMs(5);
					clientTransport.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); }, 1000)) {
				*error = "the round never started";
				return false;
			}
			// The host completes the match one frame ahead of the client's applied tick; the client holds
			// the stop for that tick boundary, exactly as the deferred-stop path requires.
			NetLockstepStop complete;
			complete.senderPeerId = 1;
			complete.reason = NetLockstepStopReason::Complete;
			complete.frame = 1;
			complete.message = "match over";
			std::vector<uint8_t> bytes;
			if (!EncodePacket({complete}, bytes, error) || !hostTransport.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			drive([&] { return false; }, 100);
			if (!client.IsRunning() || client.GetStats().framesAccepted != 0) {
				*error = "the client is not in the state this fixture needs";
				return false;
			}
			// The host starts the next match on the same start frame; the client follows it.
			NetLockstepConfig hostRestart = hostConfig;
			hostRestart.roundId = roundTwo;
			hostRestart.remoteTransportPeerId = 1;
			if (!host.Start(hostTransport, hostRestart, error)) {
				return false;
			}
			if (!drive([&] { return host.IsRunning() && client.IsRunning() && client.GetRoundId() == roundTwo; }, 4000)) {
				*error = "the client never followed the host onto the next match: " + client.BuildReportJson();
				return false;
			}
			if (!host.QueueLocalInput(0, {MakeFrame(100, 1)}, {}, error) || !client.QueueLocalInput(0, {MakeFrame(200, 1)}, {}, error)) {
				return false;
			}
			if (!drive([&] { return client.GetStats().framesAccepted == 1; }, 1000)) {
				*error = "the followed round did not commit its first frame";
				return false;
			}
			client.FinishSimulationTick(0);
			if (!client.IsRunning()) {
				*error = "the previous round's deferred Complete stopped the round the client followed: " +
				         client.GetStats().timeoutReason;
				return false;
			}
			// The deferred-stop MODE is the launch path's, not the round's: a follower that dropped it
			// would apply a stop at a point no other peer does. A Complete for a later frame is held.
			NetLockstepStop later;
			later.senderPeerId = 1;
			later.reason = NetLockstepStopReason::Complete;
			later.frame = 5;
			later.message = "later match over";
			if (!EncodePacket({later}, bytes, error) || !hostTransport.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			drive([&] { return false; }, 100);
			if (!client.IsRunning()) {
				*error = "the follower stopped applying deferred stops at the tick boundary: " + client.GetStats().timeoutReason;
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS review_readopt_clears_the_deferred_stop deferred_mode_kept" << std::endl;
			return true;
		}

		// The whole round formed, then ONE stray repeated start: the four-peer shape whose answers
		// doubled per tick on the unfixed engine. Measures what the tip actually sends in a second.
		bool TestReviewFourPeerStrayStartRate(std::string* error) {
			const uint16_t port = 43111;
			const uint64_t sessionId = 0x70000000000000B1ULL;
			const uint64_t roundId = 0x9A5E0000000000B1ULL;
			LoopbackTransport hostT, clientAT, clientBT, clientCT;
			if (!hostT.StartHost(port, error) || !clientAT.Connect("loopback", port, error) ||
			    !clientBT.Connect("loopback", port, error) || !clientCT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.startFrame = 0;
				c.inputDelayFrames = 0;
				c.timeoutMs = 60000;
				c.localPeerId = local;
				c.peerCount = 4;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.frameLane = NetTransportLane::ControlReliable;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				c.roundId = relay ? roundId : 0;
				return c;
			};
			NetLockstepCoordinator host, clientA, clientB, clientC;
			uint64_t now = 0;
			bool budgetHit = false;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 5, now += 5) {
					host.Tick(now);
					clientA.Tick(now);
					clientB.Tick(now);
					clientC.Tick(now);
					if (done()) {
						return true;
					}
					if (StartsSent({&host, &clientA, &clientB, &clientC}) > c_ReviewStartBudget) {
						budgetHit = true;
						return false;
					}
					hostT.AdvanceTimeMs(5);
					clientAT.AdvanceTimeMs(5);
					clientBT.AdvanceTimeMs(5);
					clientCT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}, {4, 3}}, true), error) ||
			    !clientA.Start(clientAT, cfg(2, {{1, 1}}, false), error) ||
			    !clientB.Start(clientBT, cfg(3, {{1, 1}}, false), error) ||
			    !clientC.Start(clientCT, cfg(4, {{1, 1}}, false), error)) {
				return false;
			}
			if (!drive([&] { return host.IsRunning() && clientA.IsRunning() && clientB.IsRunning() && clientC.IsRunning(); }, 2000)) {
				*error = "the four-peer round never started";
				return false;
			}
			const uint32_t before = StartsSent({&host, &clientA, &clientB, &clientC});
			NetLockstepStart repeat;
			repeat.sessionId = sessionId;
			repeat.startFrame = 0;
			repeat.inputDelayFrames = 0;
			repeat.controllerFrameVersion = ControllerFrame::c_Version;
			repeat.controllerFrameEncodedSize = static_cast<uint16_t>(ControllerFrame::c_EncodedSize);
			repeat.localPeerId = 2;
			repeat.peerCount = 4;
			repeat.scenario = "LockstepSelfTest";
			repeat.ownershipPolicy = "unique-id-split";
			repeat.roundId = roundId;
			std::vector<uint8_t> bytes;
			if (!EncodePacket({repeat}, bytes, error) || !clientAT.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			const uint64_t startedAt = now;
			drive([&] { return false; }, 1000);
			const uint32_t sent = StartsSent({&host, &clientA, &clientB, &clientC}) - before;
			const uint64_t elapsed = now - startedAt;
			if (sent > 40 || budgetHit) {
				*error = "one stray start in a formed four-peer round produced " + std::to_string(sent) +
				         " starts in " + std::to_string(elapsed) + "ms" + (budgetHit ? " (budget stop)" : "");
				return false;
			}
			if (host.IsFailed() || !host.IsRunning() || !clientA.IsRunning() || !clientB.IsRunning() || !clientC.IsRunning()) {
				*error = "a stray start disturbed the running four-peer round";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS review_four_peer_stray_start_rate starts=" << sent
			          << " in " << elapsed << "ms" << std::endl;
			return true;
		}
		// Following the round authority skips the stale-round rule, which used to be what kept another
		// round's start away from the FIELD validation below it. A start from the host that agrees on
		// the start frame but disagrees on any other field now fails the round with a ProtocolError
		// where the base counted it stale and played on.
		bool TestReviewAuthorityStartDoesNotFailTheRound(std::string* error) {
			const uint16_t port = 43112;
			const uint64_t sessionId = 0x70000000000000B2ULL;
			const uint64_t roundOne = 0x9A5E0000000000B2ULL;
			const uint64_t roundTwo = roundOne + 0x100ULL;
			LoopbackTransport hostTransport, clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = roundOne;
			hostConfig.timeoutMs = 60000;
			clientConfig.timeoutMs = 60000;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			uint64_t now = 0;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 5, now += 5) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					if (StartsSent({&host, &client}) > c_ReviewStartBudget) {
						return false;
					}
					hostTransport.AdvanceTimeMs(5);
					clientTransport.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); }, 1000)) {
				*error = "the round never started";
				return false;
			}
			// A start from the host for another round, agreeing on the start frame and disagreeing on
			// one other field - here the input delay a re-measured auto-delay would change.
			const uint32_t staleBefore = client.GetStats().staleRoundPackets;
			NetLockstepStart mismatched;
			mismatched.sessionId = sessionId;
			mismatched.startFrame = 0;
			mismatched.inputDelayFrames = 1;
			mismatched.controllerFrameVersion = ControllerFrame::c_Version;
			mismatched.controllerFrameEncodedSize = static_cast<uint16_t>(ControllerFrame::c_EncodedSize);
			mismatched.localPeerId = 1;
			mismatched.peerCount = 2;
			mismatched.scenario = "LockstepSelfTest";
			mismatched.ownershipPolicy = "unique-id-split";
			mismatched.roundId = roundTwo;
			std::vector<uint8_t> bytes;
			if (!EncodePacket({mismatched}, bytes, error) || !hostTransport.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			drive([&] { return false; }, 300);
			if (client.IsFailed()) {
				*error = "a start from the round authority that disagrees on a field failed the client's "
				         "round instead of being counted stale: " + client.GetStats().timeoutReason;
				return false;
			}
			if (client.GetStats().staleRoundPackets != staleBefore + 1 || client.GetRoundId() != roundOne) {
				*error = "a mismatched start was neither counted stale nor left the round alone (stale=" +
				         std::to_string(client.GetStats().staleRoundPackets) + " round moved=" +
				         std::to_string(client.GetRoundId() != roundOne) + ")";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS review_authority_start_does_not_fail_the_round" << std::endl;
			return true;
		}
		// A followed round re-sends the production the round we left carried away under its own tag. A
		// refused send on a reliable lane is backpressure, not a lost frame: the host waits for exactly
		// that frame, so what could not go out has to go out later.
		bool TestCoordinatorReadoptResendSurvivesARefusedSend(std::string* error) {
			const uint16_t port = 43104;
			const uint64_t sessionId = 0x70000000000000A4ULL;
			const uint64_t roundOne = 0x9A5E0000000000A4ULL;
			const uint64_t roundTwo = roundOne + 0x100ULL;
			NetLockstepFrame resent{2, 0, {MakeFrame(200, 1)}, {}, roundTwo, {}};
			LoopbackTransport hostTransport;
			RecoveryTapTransport clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = roundOne;
			hostConfig.timeoutMs = 60000;
			clientConfig.timeoutMs = 60000;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			uint64_t now = 0;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 5, now += 5) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					if (StartsSent({&host, &client}) > c_RoundStartBudget) {
						return false;
					}
					hostTransport.AdvanceTimeMs(5);
					clientTransport.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); }, 1000)) {
				*error = "the first round never started";
				return false;
			}
			// The client produces a frame into the round it is about to lose.
			if (!client.QueueLocalInput(0, {MakeFrame(200, 1)}, {}, error)) {
				return false;
			}
			drive([&] { return false; }, 50);
			// Its link refuses everything, so the follow's start and its re-sent production are refused.
			LoopbackTransportConfig refuse;
			refuse.refuseSendsToPeer = 1;
			clientTransport.SetFaultConfig(refuse);
			NetLockstepConfig hostRestart = hostConfig;
			hostRestart.roundId = roundTwo;
			hostRestart.remoteTransportPeerId = 1;
			if (!host.Start(hostTransport, hostRestart, error)) {
				return false;
			}
			if (!drive([&] { return client.GetRoundId() == roundTwo; }, 2000)) {
				*error = "the client never followed the host onto the new round";
				return false;
			}
			drive([&] { return false; }, 500);
			if (!CheckRecoveredInput(clientTransport, resent, sessionId, false, error)) return false;
			if (host.GetStats().framePacketsReceived != 0) {
				*error = "the fixture did not refuse the re-send it is about to test";
				return false;
			}
			clientTransport.SetFaultConfig(LoopbackTransportConfig{});
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); }, 4000)) {
				*error = "the round never re-formed once the link freed up: " + client.BuildReportJson();
				return false;
			}
			if (!host.QueueLocalInput(0, {MakeFrame(100, 1)}, {}, error)) {
				return false;
			}
			if (!drive([&] { return host.GetStats().framesAccepted == 1 && client.GetStats().framesAccepted == 1; }, 2000)) {
				*error = "the refused re-send never reached the host: host=" + host.BuildReportJson();
				return false;
			}
			if (!CheckRecoveredInput(clientTransport, resent, sessionId, true, error)) return false;
			std::cout << "[net-lockstep-selftest] PASS readopt_resend_survives_a_refused_send frames_sent="
			          << client.GetStats().framePacketsSent << std::endl;
			return true;
		}
		// A send buffer that fits the follow's start and not the frame behind it leaves the follower owing
		// one frame - and the follower can commit that frame itself, from its own copy and the host's, long
		// before the retry goes out. The host still needs it, so what is owed is kept whole.
		bool TestCoordinatorOwedFrameOutlivesItsLocalCommit(std::string* error) {
			const uint16_t port = 43105;
			const uint64_t sessionId = 0x70000000000000A5ULL;
			const uint64_t roundOne = 0x9A5E0000000000A5ULL;
			const uint64_t roundTwo = roundOne + 0x100ULL;
			NetLockstepFrame resent{2, 0, {MakeFrame(200, 1)}, {}, roundTwo, {}};
			LoopbackTransport hostTransport;
			RecoveryTapTransport clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = roundOne;
			hostConfig.timeoutMs = 60000;
			clientConfig.timeoutMs = 60000;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			uint64_t now = 0;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 5, now += 5) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					if (StartsSent({&host, &client}) > c_RoundStartBudget) {
						return false;
					}
					hostTransport.AdvanceTimeMs(5);
					clientTransport.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); }, 1000)) {
				*error = "the first round never started";
				return false;
			}
			if (!client.QueueLocalInput(0, {MakeFrame(200, 1)}, {}, error)) {
				return false;
			}
			drive([&] { return false; }, 50);
			// The link takes the follow's start and refuses everything after it.
			LoopbackTransportConfig refuse;
			refuse.refuseSendsToPeer = 1;
			refuse.acceptedSendsBeforeRefusing = 1;
			clientTransport.SetFaultConfig(refuse);
			NetLockstepConfig hostRestart = hostConfig;
			hostRestart.roundId = roundTwo;
			hostRestart.remoteTransportPeerId = 1;
			if (!host.Start(hostTransport, hostRestart, error)) {
				return false;
			}
			if (!drive([&] { return host.IsRunning() && client.IsRunning() && client.GetRoundId() == roundTwo; }, 4000)) {
				*error = "the round never re-formed on the start the link did take: " + client.BuildReportJson();
				return false;
			}
			if (!CheckRecoveredInput(clientTransport, resent, sessionId, false, error)) return false;
			if (host.GetStats().framePacketsReceived != 0) {
				*error = "the fixture did not refuse the owed frame it is about to test";
				return false;
			}
			// The host's own frame arrives, so the follower commits the very frame it still owes.
			if (!host.QueueLocalInput(0, {MakeFrame(100, 1)}, {}, error)) {
				return false;
			}
			if (!drive([&] { return client.GetStats().framesAccepted == 1; }, 2000)) {
				*error = "the follower never committed the frame it owes";
				return false;
			}
			clientTransport.SetFaultConfig(LoopbackTransportConfig{});
			if (!drive([&] { return host.GetStats().framesAccepted == 1; }, 4000)) {
				*error = "an owed frame the follower had already committed was never sent: host=" + host.BuildReportJson();
				return false;
			}
			if (!CheckRecoveredInput(clientTransport, resent, sessionId, true, error)) return false;
			std::cout << "[net-lockstep-selftest] PASS owed_frame_outlives_its_local_commit frames_sent="
			          << client.GetStats().framePacketsSent << std::endl;
			return true;
		}
		// A round that has failed sends nothing more. The re-send drains from the tick, so an owed frame
		// left over from a follow must not keep going out into a round this peer has stopped.
		bool TestCoordinatorFailedRoundStopsResending(std::string* error) {
			const uint16_t port = 43106;
			const uint64_t sessionId = 0x70000000000000A6ULL;
			const uint64_t roundOne = 0x9A5E0000000000A6ULL;
			const uint64_t roundTwo = roundOne + 0x100ULL;
			NetLockstepFrame resent{2, 0, {MakeFrame(200, 1)}, {}, roundTwo, {}};
			LoopbackTransport hostTransport;
			RecoveryTapTransport clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = roundOne;
			hostConfig.timeoutMs = 60000;
			clientConfig.timeoutMs = 60000;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			uint64_t now = 0;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 5, now += 5) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					if (StartsSent({&host, &client}) > c_RoundStartBudget) {
						return false;
					}
					hostTransport.AdvanceTimeMs(5);
					clientTransport.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); }, 1000)) {
				*error = "the first round never started";
				return false;
			}
			if (!client.QueueLocalInput(0, {MakeFrame(200, 1)}, {}, error)) {
				return false;
			}
			drive([&] { return false; }, 50);
			LoopbackTransportConfig refuse;
			refuse.refuseSendsToPeer = 1;
			refuse.acceptedSendsBeforeRefusing = 1;
			clientTransport.SetFaultConfig(refuse);
			NetLockstepConfig hostRestart = hostConfig;
			hostRestart.roundId = roundTwo;
			hostRestart.remoteTransportPeerId = 1;
			if (!host.Start(hostTransport, hostRestart, error)) {
				return false;
			}
			if (!drive([&] { return client.GetRoundId() == roundTwo; }, 4000)) {
				*error = "the client never followed the host onto the new round";
				return false;
			}
			if (!CheckRecoveredInput(clientTransport, resent, sessionId, false, error)) return false;
			// The round fails while a frame is still owed.
			NetLockstepStop stop;
			stop.senderPeerId = 1;
			stop.reason = NetLockstepStopReason::ProtocolError;
			stop.frame = 0;
			stop.message = "host gave up";
			std::vector<uint8_t> bytes;
			if (!EncodePacket({stop}, bytes, error) || !hostTransport.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			if (!drive([&] { return client.IsFailed(); }, 1000)) {
				*error = "the client's round never failed, so this fixture proves nothing";
				return false;
			}
			const uint32_t sentAtFailure = client.GetStats().framePacketsSent;
			const size_t attemptsAtStop = clientTransport.sendAttempts;
			clientTransport.SetFaultConfig(LoopbackTransportConfig{});
			drive([&] { return false; }, 1000);
			if (client.GetStats().framePacketsSent != sentAtFailure || clientTransport.sendAttempts != attemptsAtStop) {
				*error = "a failed round kept re-sending: frame_packets_sent " + std::to_string(sentAtFailure) +
				         " -> " + std::to_string(client.GetStats().framePacketsSent);
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS failed_round_stops_resending frames_sent=" << sentAtFailure << std::endl;
			return true;
		}
		bool TestCoordinatorOwedFrameRetryEndsWithTheRound(std::string* error) {
			const uint16_t port = 43130;
			const uint64_t sessionId = 0x70000000000000D0ULL;
			const uint64_t roundOne = 0x9A5E0000000000D0ULL;
			const uint64_t roundTwo = roundOne + 0x100ULL;
			NetLockstepFrame resent{2, 0, {MakeFrame(200, 1)}, {}, roundTwo, {}};
			LoopbackTransport hostTransport;
			RecoveryTapTransport clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = roundOne;
			hostConfig.timeoutMs = 500;
			clientConfig.timeoutMs = 500;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			uint64_t now = 0;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 5, now += 5) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					if (StartsSent({&host, &client}) > c_RoundStartBudget) {
						return false;
					}
					hostTransport.AdvanceTimeMs(5);
					clientTransport.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); }, 1000)) {
				*error = "the round never started";
				return false;
			}
			if (!client.QueueLocalInput(0, {MakeFrame(200, 1)}, {}, error)) {
				return false;
			}
			drive([&] { return false; }, 50);
			LoopbackTransportConfig refuse;
			refuse.refuseSendsToPeer = 1;
			refuse.acceptedSendsBeforeRefusing = 1;
			clientTransport.SetFaultConfig(refuse);
			NetLockstepConfig hostRestart = hostConfig;
			hostRestart.roundId = roundTwo;
			hostRestart.remoteTransportPeerId = 1;
			if (!host.Start(hostTransport, hostRestart, error)) {
				return false;
			}
			if (!drive([&] { return client.GetRoundId() == roundTwo && client.IsRunning() && host.IsRunning(); }, 4000)) {
				*error = "the client never followed on the start the link took";
				return false;
			}
			if (!CheckRecoveredInput(clientTransport, resent, sessionId, false, error)) return false;
			if (host.GetStats().framePacketsReceived != 0) {
				*error = "the fixture did not refuse the owed frame";
				return false;
			}
			if (!host.QueueLocalInput(0, {MakeFrame(100, 1)}, {}, error)) {
				return false;
			}
			if (!drive([&] { return client.GetStats().framesAccepted == 1; }, 2000)) {
				*error = "the follower never committed the frame it owes";
				return false;
			}
			// The client's own grace is 500ms and nothing is pending on it, so it keeps retrying and
			// stays Running. The host, which IS waiting, gives up on its own grace.
			if (!drive([&] { return !host.IsRunning(); }, 4000)) {
				*error = "the host never gave up on the frame the link refuses";
				return false;
			}
			if (!drive([&] { return !client.IsRunning(); }, 2000)) {
				*error = "the host's stop never reached the follower: " + client.BuildReportJson();
				return false;
			}
			const uint32_t sentAtStop = client.GetStats().framePacketsSent;
			const size_t attemptsAtStop = clientTransport.sendAttempts;
			clientTransport.SetFaultConfig(LoopbackTransportConfig{});
			drive([&] { return false; }, 2000);
			if (client.GetStats().framePacketsSent != sentAtStop || clientTransport.sendAttempts != attemptsAtStop) {
				*error = "the retry outlived the round: frame_packets_sent " + std::to_string(sentAtStop) +
				         " -> " + std::to_string(client.GetStats().framePacketsSent);
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS owed_frame_retry_ends_with_the_round frames_sent="
			          << sentAtStop << " client=" << NetLockstepCoordinator::StateName(client.GetState()) << std::endl;
			return true;
		}

		bool TestCoordinatorOwedFrameKeepsItsCommands(std::string* error) {
			const uint16_t port = 43131;
			const uint64_t sessionId = 0x70000000000000D1ULL;
			const uint64_t roundOne = 0x9A5E0000000000D1ULL;
			const uint64_t roundTwo = roundOne + 0x100ULL;
			NetLockstepFrame resent{2, 0, {MakeFrame(200, 1)}, {}, roundTwo, {}};
			LoopbackTransport hostTransport;
			RecoveryTapTransport clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = roundOne;
			hostConfig.timeoutMs = 60000;
			clientConfig.timeoutMs = 60000;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			uint64_t now = 0;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 5, now += 5) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					if (StartsSent({&host, &client}) > c_RoundStartBudget) {
						return false;
					}
					hostTransport.AdvanceTimeMs(5);
					clientTransport.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); }, 1000)) {
				*error = "the round never started";
				return false;
			}
			const NetGameCommand queued{2, NetGameSetTeamFunds{1, 4200}};
			resent.commands = {queued};
			resent.observations = MakeObservationSet(2, 12, 71, 0.25F);
			if (!client.QueueLocalInput(0, {MakeFrame(200, 1)}, {queued}, error, resent.observations)) {
				return false;
			}
			drive([&] { return false; }, 50);
			LoopbackTransportConfig refuse;
			refuse.refuseSendsToPeer = 1;
			refuse.acceptedSendsBeforeRefusing = 1;
			clientTransport.SetFaultConfig(refuse);
			NetLockstepConfig hostRestart = hostConfig;
			hostRestart.roundId = roundTwo;
			hostRestart.remoteTransportPeerId = 1;
			if (!host.Start(hostTransport, hostRestart, error)) {
				return false;
			}
			if (!drive([&] { return client.GetRoundId() == roundTwo && client.IsRunning() && host.IsRunning(); }, 4000)) {
				*error = "the client never followed on the start the link took";
				return false;
			}
			if (!CheckRecoveredInput(clientTransport, resent, sessionId, false, error)) return false;
			if (host.GetStats().framePacketsReceived != 0) {
				*error = "the fixture did not refuse the owed frame";
				return false;
			}
			if (!host.QueueLocalInput(0, {MakeFrame(100, 1)}, {}, error)) {
				return false;
			}
			if (!drive([&] { return client.GetStats().framesAccepted == 1; }, 2000)) {
				*error = "the follower never committed the frame it owes";
				return false;
			}
			NetLockstepReadyFrame clientReady;
			if (!client.PopReadyFrame(clientReady) || clientReady.frame != 0 ||
			    clientReady.localFrames.size() != 1 ||
			    ControllerFrameCodec::Encode(clientReady.localFrames.front()) != ControllerFrameCodec::Encode(MakeFrame(200, 1)) ||
			    clientReady.localCommands != std::vector<NetGameCommand>{queued} || clientReady.localObservations != resent.observations) {
				*error = "the follower committed different input or commands";
				return false;
			}
			clientTransport.SetFaultConfig(LoopbackTransportConfig{});
			std::vector<NetLockstepReadyFrame> hostReady;
			if (!drive([&] {
					NetLockstepReadyFrame ready;
					while (host.PopReadyFrame(ready)) {
						hostReady.push_back(ready);
					}
					return hostReady.size() == 1;
				}, 4000)) {
				*error = "the owed frame never reached the host: " + host.BuildReportJson();
				return false;
			}
			if (hostReady[0].frame != clientReady.frame ||
			    hostReady[0].remoteFrames.size() != 1 ||
			    ControllerFrameCodec::Encode(hostReady[0].remoteFrames.front()) != ControllerFrameCodec::Encode(clientReady.localFrames.front()) ||
			    hostReady[0].remoteCommands != clientReady.localCommands || hostReady[0].remoteObservations != clientReady.localObservations ||
			    client.GetStats().observationsCarried != 0 || client.GetStats().observationsDropped != 0 ||
			    host.GetStats().observationsCarried != 0 || host.GetStats().observationsDropped != 0) {
				*error = "the owed frame reached the host carrying " +
				         std::to_string(hostReady[0].remoteCommands.size()) + " of its 1 command";
				return false;
			}
			if (!CheckRecoveredInput(clientTransport, resent, sessionId, true, error)) return false;
			std::cout << "[net-lockstep-selftest] PASS owed_frame_keeps_its_commands" << std::endl;
			return true;
		}

		bool TestCoordinatorStoppedRoundStopsResending(std::string* error) {
			const uint16_t port = 43132;
			const uint64_t sessionId = 0x70000000000000D2ULL;
			const uint64_t roundOne = 0x9A5E0000000000D2ULL;
			const uint64_t roundTwo = roundOne + 0x100ULL;
			NetLockstepFrame resent{2, 0, {MakeFrame(200, 1)}, {}, roundTwo, {}};
			LoopbackTransport hostTransport;
			RecoveryTapTransport clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, sessionId, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, sessionId, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = roundOne;
			hostConfig.timeoutMs = 60000;
			clientConfig.timeoutMs = 60000;
			if (!StartCoordinatorPair(port, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			uint64_t now = 0;
			auto drive = [&](const std::function<bool()>& done, uint64_t maxMs) {
				for (uint64_t elapsed = 0; elapsed <= maxMs; elapsed += 5, now += 5) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					if (StartsSent({&host, &client}) > c_RoundStartBudget) {
						return false;
					}
					hostTransport.AdvanceTimeMs(5);
					clientTransport.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); }, 1000)) {
				*error = "the round never started";
				return false;
			}
			if (!client.QueueLocalInput(0, {MakeFrame(200, 1)}, {}, error)) {
				return false;
			}
			drive([&] { return false; }, 50);
			LoopbackTransportConfig refuse;
			refuse.refuseSendsToPeer = 1;
			refuse.acceptedSendsBeforeRefusing = 1;
			clientTransport.SetFaultConfig(refuse);
			NetLockstepConfig hostRestart = hostConfig;
			hostRestart.roundId = roundTwo;
			hostRestart.remoteTransportPeerId = 1;
			if (!host.Start(hostTransport, hostRestart, error)) {
				return false;
			}
			if (!drive([&] { return client.GetRoundId() == roundTwo && client.IsRunning() && host.IsRunning(); }, 4000)) {
				*error = "the client never followed on the start the link took";
				return false;
			}
			if (!CheckRecoveredInput(clientTransport, resent, sessionId, false, error)) return false;
			if (host.GetStats().framePacketsReceived != 0) {
				*error = "the fixture did not refuse the owed frame";
				return false;
			}
			const uint32_t sentWhileOwing = client.GetStats().framePacketsSent;
			// The match ends between two flushes: the round is Stopped, not Failed.
			NetLockstepStop complete;
			complete.senderPeerId = 1;
			complete.reason = NetLockstepStopReason::Complete;
			complete.frame = 1;
			complete.message = "match over";
			std::vector<uint8_t> bytes;
			if (!EncodePacket({complete}, bytes, error) || !hostTransport.Send(1, NetTransportLane::ControlReliable, bytes, error)) {
				return false;
			}
			if (!drive([&] { return client.GetState() == NetLockstepState::Stopped; }, 1000)) {
				*error = "the client never stopped on the host's Complete";
				return false;
			}
			const size_t attemptsAtStop = clientTransport.sendAttempts;
			clientTransport.SetFaultConfig(LoopbackTransportConfig{});
			drive([&] { return false; }, 2000);
			if (client.GetStats().framePacketsSent != sentWhileOwing || clientTransport.sendAttempts != attemptsAtStop) {
				*error = "a stopped round kept re-sending: frame_packets_sent " + std::to_string(sentWhileOwing) +
				         " -> " + std::to_string(client.GetStats().framePacketsSent);
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS stopped_round_stops_resending frames_sent="
			          << sentWhileOwing << std::endl;
			return true;
		}

		bool TestHoldPauseCommitsNothing(std::string* error) {
			const uint16_t port = 43080;
			LoopbackTransport hostT, leaverT, stayerT;
			if (!hostT.StartHost(port, error) || !leaverT.Connect("loopback", port, error) || !stayerT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = 0x7000000000000080ULL;
				c.timeoutMs = 5000;
				c.localPeerId = local;
				c.peerCount = 3;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			NetLockstepCoordinator host, leaver, stayer;
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error) ||
			    !leaver.Start(leaverT, cfg(2, {{1, 1}}, false), error) ||
			    !stayer.Start(stayerT, cfg(3, {{1, 1}}, false), error)) {
				return false;
			}
			uint64_t now = 0;
			auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
				for (const uint64_t until = now + forMs; now <= until; now += 5) {
					host.Tick(now);
					leaver.Tick(now);
					stayer.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					leaverT.AdvanceTimeMs(5);
					stayerT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive(2000, [&] { return host.IsRunning() && leaver.IsRunning() && stayer.IsRunning(); })) {
				*error = "hold-pause commit fixture never started";
				return false;
			}
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !leaver.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error) ||
				    !stayer.QueueLocalInput(f, {MakeFrame(300, f + 1)}, {}, error)) {
					return false;
				}
			}
			NetLockstepReadyFrame ready;
			size_t committed = 0;
			if (!drive(2000, [&] {
					while (host.PopReadyFrame(ready)) {
						++committed;
					}
					while (stayer.PopReadyFrame(ready)) {
					}
					return committed >= 2;
				})) {
				*error = "hold-pause commit fixture never committed";
				return false;
			}
			const uint64_t frozen = host.GetStats().nextFrame;
			leaverT.Stop();
			if (!drive(2000, [&] { return host.GetPeerLeaveFrames().count(2) != 0 && host.AnyDroppedSeatHeld(); })) {
				*error = "the drop never paused the coordinator";
				return false;
			}
			for (uint64_t f = frozen; f < frozen + 3; ++f) {
				(void)host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error);
				(void)stayer.QueueLocalInput(f, {MakeFrame(300, f + 1)}, {}, error);
			}
			size_t extra = 0;
			drive(500, [&] {
				while (host.PopReadyFrame(ready)) {
					++extra;
				}
				return false;
			});
			if (extra != 0 || host.GetStats().nextFrame != frozen) {
				*error = "the coordinator committed while a dropped seat was held";
				return false;
			}
			return true;
		}

		bool TestHoldExpiredResumesWithoutSeat(std::string* error) {
			const uint16_t port = 43081;
			LoopbackTransport hostT, leaverT, stayerT;
			if (!hostT.StartHost(port, error) || !leaverT.Connect("loopback", port, error) || !stayerT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = 0x7000000000000081ULL;
				c.timeoutMs = 5000;
				c.localPeerId = local;
				c.peerCount = 3;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			NetLockstepCoordinator host, leaver, stayer;
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error) ||
			    !leaver.Start(leaverT, cfg(2, {{1, 1}}, false), error) ||
			    !stayer.Start(stayerT, cfg(3, {{1, 1}}, false), error)) {
				return false;
			}
			uint64_t now = 0;
			auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
				for (const uint64_t until = now + forMs; now <= until; now += 5) {
					host.Tick(now);
					leaver.Tick(now);
					stayer.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					leaverT.AdvanceTimeMs(5);
					stayerT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive(2000, [&] { return host.IsRunning() && leaver.IsRunning() && stayer.IsRunning(); })) {
				*error = "expired-resume fixture never started";
				return false;
			}
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !leaver.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error) ||
				    !stayer.QueueLocalInput(f, {MakeFrame(300, f + 1)}, {}, error)) {
					return false;
				}
			}
			NetLockstepReadyFrame ready;
			size_t committed = 0;
			if (!drive(2000, [&] {
					while (host.PopReadyFrame(ready)) {
						++committed;
					}
					while (stayer.PopReadyFrame(ready)) {
					}
					return committed >= 2;
				})) {
				*error = "expired-resume fixture never committed";
				return false;
			}
			leaverT.Stop();
			if (!drive(2000, [&] { return host.GetPeerLeaveFrames().count(2) != 0; })) {
				*error = "expired-resume drop was never recorded";
				return false;
			}
			const uint64_t leaveFrame = host.GetPeerLeaveFrames().at(2);
			host.ResolveHeldSeat(2, NetLockstepHoldResolution::Expired, now);
			if (!drive(2000, [&] { return !host.AnyDroppedSeatHeld() && !stayer.AnyDroppedSeatHeld(); })) {
				*error = "Expired never cleared the held seat on both peers";
				return false;
			}
			if (host.HeldSeatResolution(2) != NetLockstepHoldResolution::Expired) {
				*error = "the host did not record Expired";
				return false;
			}
			for (uint64_t f = leaveFrame; f < leaveFrame + 3; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !stayer.QueueLocalInput(f, {MakeFrame(300, f + 1)}, {}, error)) {
					return false;
				}
			}
			size_t extra = 0;
			if (!drive(2000, [&] {
					while (host.PopReadyFrame(ready)) {
						++extra;
					}
					while (stayer.PopReadyFrame(ready)) {
					}
					return extra >= 3;
				})) {
				*error = "commits did not resume after Expired";
				return false;
			}
			if (host.GetStats().nextFrame <= leaveFrame) {
				*error = "the expired seat was still required after resume";
				return false;
			}
			return true;
		}

		bool TestHoldReclaimedResyncsAtLeaveFrame(std::string* error) {
			const uint16_t port = 43082;
			LoopbackTransport hostT, leaverT, stayerT;
			if (!hostT.StartHost(port, error) || !leaverT.Connect("loopback", port, error) || !stayerT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = 0x7000000000000082ULL;
				c.timeoutMs = 5000;
				c.localPeerId = local;
				c.peerCount = 3;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			NetLockstepCoordinator host, leaver, stayer;
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error) ||
			    !leaver.Start(leaverT, cfg(2, {{1, 1}}, false), error) ||
			    !stayer.Start(stayerT, cfg(3, {{1, 1}}, false), error)) {
				return false;
			}
			uint64_t now = 0;
			auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
				for (const uint64_t until = now + forMs; now <= until; now += 5) {
					host.Tick(now);
					leaver.Tick(now);
					stayer.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					leaverT.AdvanceTimeMs(5);
					stayerT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive(2000, [&] { return host.IsRunning() && leaver.IsRunning() && stayer.IsRunning(); })) {
				*error = "reclaim-resync fixture never started";
				return false;
			}
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !leaver.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error) ||
				    !stayer.QueueLocalInput(f, {MakeFrame(300, f + 1)}, {}, error)) {
					return false;
				}
			}
			NetLockstepReadyFrame ready;
			size_t committed = 0;
			if (!drive(2000, [&] {
					while (host.PopReadyFrame(ready)) {
						++committed;
					}
					return committed >= 2;
				})) {
				*error = "reclaim-resync fixture never committed";
				return false;
			}
			leaverT.Stop();
			if (!drive(2000, [&] { return host.GetPeerLeaveFrames().count(2) != 0; })) {
				*error = "reclaim-resync drop was never recorded";
				return false;
			}
			const uint64_t leaveFrame = host.GetPeerLeaveFrames().at(2);
			if (host.GetStats().nextFrame != leaveFrame) {
				*error = "the pause did not freeze at the leave frame";
				return false;
			}
			host.ResolveHeldSeat(2, NetLockstepHoldResolution::Reclaimed, now);
			if (host.GetStats().nextFrame != leaveFrame) {
				*error = "Reclaimed moved the stop frame off the leave frame";
				return false;
			}
			if (host.GetStats().timeoutReason.find("ResyncRequested") == std::string::npos) {
				*error = "Reclaimed did not resume through the resync path: " + host.GetStats().timeoutReason;
				return false;
			}
			return true;
		}

		// Production DefersStops, so Reclaimed must request resync on this tick. A 2-peer Tick
		// that treats the emptied hold as "nobody coming back" Stops with PeerLeft instead.
		bool DriveTwoPeerHoldResolution(uint16_t port, uint64_t sessionId, NetLockstepHoldResolution resolution,
		                                NetLockstepCoordinator& host, LoopbackTransport& hostT, LoopbackTransport& clientT,
		                                uint64_t& leaveFrame, uint64_t& now, std::string* error) {
			if (!hostT.StartHost(port, error) || !clientT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = sessionId;
				c.timeoutMs = 5000;
				c.localPeerId = local;
				c.peerCount = 2;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				c.matchConfig.players = {{1, 0, false, "Host"}, {2, 1, false, "Client"}};
				return c;
			};
			NetLockstepCoordinator client;
			if (!host.Start(hostT, cfg(1, {{2, 1}}, true), error) || !client.Start(clientT, cfg(2, {{1, 1}}, false), error)) {
				return false;
			}
			host.DeferStopsToTickBoundary();
			now = 0;
			auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
				for (const uint64_t until = now + forMs; now <= until; now += 5) {
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					clientT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive(2000, [&] { return host.IsRunning() && client.IsRunning(); })) {
				*error = "two-peer hold fixture never started";
				return false;
			}
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !client.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error)) {
					return false;
				}
			}
			NetLockstepReadyFrame ready;
			size_t committed = 0;
			if (!drive(2000, [&] {
					while (host.PopReadyFrame(ready)) {
						++committed;
					}
					return committed >= 2;
				})) {
				*error = "two-peer hold fixture never committed";
				return false;
			}
			clientT.Stop();
			if (!drive(2000, [&] { return host.GetPeerLeaveFrames().count(2) != 0 && host.AnyDroppedSeatHeld(); })) {
				*error = "two-peer hold fixture never recorded the drop";
				return false;
			}
			leaveFrame = host.GetPeerLeaveFrames().at(2);
			host.ResolveHeldSeat(2, resolution, now);
			host.Tick(now + 5);
			host.FinishSimulationTick(leaveFrame > 0 ? leaveFrame - 1 : 0);
			return true;
		}

		bool TestTwoPeerReclaimedRequestsResync(std::string* error) {
			LoopbackTransport hostT, clientT;
			NetLockstepCoordinator host;
			uint64_t leaveFrame = 0;
			uint64_t now = 0;
			if (!DriveTwoPeerHoldResolution(43086, 0x7000000000000086ULL, NetLockstepHoldResolution::Reclaimed, host, hostT, clientT, leaveFrame, now, error)) {
				return false;
			}
			if (host.IsStopped()) {
				*error = "Reclaimed stopped a 2-peer round: " + host.GetStats().timeoutReason;
				return false;
			}
			if (host.GetStats().timeoutReason.find("ResyncRequested") == std::string::npos) {
				*error = "Reclaimed did not request resync: " + host.GetStats().timeoutReason;
				return false;
			}
			if (host.GetStats().nextFrame != leaveFrame) {
				*error = "the ResyncRequested stop moved off the held frame";
				return false;
			}
			return true;
		}

		bool TestTwoPeerSubstitutedRequestsResync(std::string* error) {
			LoopbackTransport hostT, clientT;
			NetLockstepCoordinator host;
			uint64_t leaveFrame = 0;
			uint64_t now = 0;
			if (!DriveTwoPeerHoldResolution(43087, 0x7000000000000087ULL, NetLockstepHoldResolution::Substituted, host, hostT, clientT, leaveFrame, now, error)) {
				return false;
			}
			if (host.IsStopped()) {
				*error = "Substituted stopped a 2-peer round: " + host.GetStats().timeoutReason;
				return false;
			}
			if (host.GetStats().timeoutReason.find("ResyncRequested") == std::string::npos) {
				*error = "Substituted did not request resync: " + host.GetStats().timeoutReason;
				return false;
			}
			if (host.GetStats().nextFrame != leaveFrame) {
				*error = "the Substituted resync moved off the held frame";
				return false;
			}
			return true;
		}

		bool TestTwoPeerExpiredEndsLastPlayer(std::string* error) {
			LoopbackTransport hostT, clientT;
			NetLockstepCoordinator host;
			uint64_t leaveFrame = 0;
			uint64_t now = 0;
			if (!DriveTwoPeerHoldResolution(43088, 0x7000000000000088ULL, NetLockstepHoldResolution::Expired, host, hostT, clientT, leaveFrame, now, error)) {
				return false;
			}
			if (!host.IsStopped() || host.GetStats().timeoutReason.find("PeerLeft") == std::string::npos) {
				*error = "Expired did not stop the last-player round: " + host.GetStats().timeoutReason;
				return false;
			}
			return true;
		}

		bool TestThreePeerReclaimedRequestsResync(std::string* error) {
			const uint16_t port = 43089;
			LoopbackTransport hostT, leaverT, stayerT;
			if (!hostT.StartHost(port, error) || !leaverT.Connect("loopback", port, error) || !stayerT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = 0x7000000000000089ULL;
				c.timeoutMs = 5000;
				c.localPeerId = local;
				c.peerCount = 3;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			NetLockstepCoordinator host, leaver, stayer;
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error) ||
			    !leaver.Start(leaverT, cfg(2, {{1, 1}}, false), error) ||
			    !stayer.Start(stayerT, cfg(3, {{1, 1}}, false), error)) {
				return false;
			}
			host.DeferStopsToTickBoundary();
			uint64_t now = 0;
			auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
				for (const uint64_t until = now + forMs; now <= until; now += 5) {
					host.Tick(now);
					leaver.Tick(now);
					stayer.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					leaverT.AdvanceTimeMs(5);
					stayerT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive(2000, [&] { return host.IsRunning() && leaver.IsRunning() && stayer.IsRunning(); })) {
				*error = "three-peer reclaim fixture never started";
				return false;
			}
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !leaver.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error) ||
				    !stayer.QueueLocalInput(f, {MakeFrame(300, f + 1)}, {}, error)) {
					return false;
				}
			}
			NetLockstepReadyFrame ready;
			size_t committed = 0;
			if (!drive(2000, [&] {
					while (host.PopReadyFrame(ready)) {
						++committed;
					}
					return committed >= 2;
				})) {
				*error = "three-peer reclaim fixture never committed";
				return false;
			}
			leaverT.Stop();
			if (!drive(2000, [&] { return host.GetPeerLeaveFrames().count(2) != 0 && host.AnyDroppedSeatHeld(); })) {
				*error = "three-peer reclaim fixture never recorded the drop";
				return false;
			}
			host.ResolveHeldSeat(2, NetLockstepHoldResolution::Reclaimed, now);
			host.Tick(now + 5);
			if (host.IsStopped()) {
				*error = "Reclaimed stopped a 3-peer round: " + host.GetStats().timeoutReason;
				return false;
			}
			if (host.GetStats().timeoutReason.find("ResyncRequested") == std::string::npos) {
				*error = "3-peer Reclaimed did not request resync: " + host.GetStats().timeoutReason;
				return false;
			}
			return true;
		}

		bool TestResyncRoundCommitsAfterReclaimed(std::string* error) {
			LoopbackTransport hostT, clientT;
			NetLockstepCoordinator host;
			uint64_t leaveFrame = 0;
			uint64_t now = 0;
			if (!DriveTwoPeerHoldResolution(43090, 0x7000000000000090ULL, NetLockstepHoldResolution::Reclaimed, host, hostT, clientT, leaveFrame, now, error)) {
				return false;
			}
			RecoveryWireTransport resyncHostT, resyncClientT;
			if (!resyncHostT.StartHost(43091, error) || !resyncClientT.Connect("loopback", 43091, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = 0x7000000000000091ULL;
				c.roundId = relay ? 0x7000000000000092ULL : 0;
				c.startFrame = ScenarioRunner::ResyncResumeStartFrame(leaveFrame);
				c.resumeFromSnapshot = true;
				c.timeoutMs = 5000;
				c.localPeerId = local;
				c.peerCount = 2;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				c.matchConfig.players = {{1, 0, false, "Host"}, {2, 1, false, "Client"}};
				return c;
			};
			NetLockstepCoordinator resyncHost, resyncClient;
			if (!resyncHost.Start(resyncHostT, cfg(1, {{2, 1}}, true), error) ||
			    !resyncClient.Start(resyncClientT, cfg(2, {{1, 1}}, false), error)) {
				return false;
			}
			NetLockstepStop leftover;
			leftover.senderPeerId = 2;
			leftover.reason = NetLockstepStopReason::PeerDropped;
			leftover.frame = leaveFrame;
			leftover.message = "connection lost";
			std::vector<uint8_t> leftoverBytes;
			if (!EncodePacket({leftover}, leftoverBytes, error)) {
				return false;
			}
			resyncHostT.injected.push_back({NetTransportEventType::PeerDisconnected, 1, NetTransportLane::ControlReliable, {}, "connection lost"});
			resyncHostT.injected.push_back({NetTransportEventType::PacketReceived, 1, NetTransportLane::ControlReliable, leftoverBytes, {}});
			auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
				for (const uint64_t until = now + forMs; now <= until; now += 5) {
					resyncHost.Tick(now);
					resyncClient.Tick(now);
					if (done()) {
						return true;
					}
					resyncHostT.AdvanceTimeMs(5);
					resyncClientT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive(2000, [&] { return resyncHost.IsRunning() && resyncClient.IsRunning(); })) {
				*error = "resync round after reclaim never started";
				return false;
			}
			if (resyncHost.GetConfig().startFrame != leaveFrame || resyncHost.GetStats().effectiveStartFrame != leaveFrame) {
				*error = "resync first frame is " + std::to_string(resyncHost.GetStats().effectiveStartFrame) +
				         " not drop frame " + std::to_string(leaveFrame);
				return false;
			}
			if (!resyncHost.PrimeResyncInputs({}, error) || !resyncClient.PrimeResyncInputs({}, error)) {
				return false;
			}
			if (resyncHost.GetPeerLeaveFrames().count(2) != 0 || resyncHost.AnyDroppedSeatHeld()) {
				*error = "resync round re-held the returner's seat";
				return false;
			}
			if (!resyncHost.QueueLocalInput(leaveFrame, {MakeFrame(100, 3)}, {}, error) ||
			    !resyncClient.QueueLocalInput(leaveFrame, {MakeFrame(200, 3)}, {}, error) ||
			    !resyncHost.QueueLocalInput(leaveFrame + 1, {MakeFrame(100, 4)}, {}, error) ||
			    !resyncClient.QueueLocalInput(leaveFrame + 1, {MakeFrame(200, 4)}, {}, error)) {
				return false;
			}
			NetLockstepReadyFrame ready;
			std::vector<uint64_t> hostFrames;
			size_t clientCommitted = 0;
			if (!drive(2000, [&] {
					while (resyncHost.PopReadyFrame(ready)) {
						hostFrames.push_back(ready.frame);
					}
					while (resyncClient.PopReadyFrame(ready)) {
						++clientCommitted;
					}
					return hostFrames.size() >= 2 && clientCommitted >= 2;
				})) {
				*error = "resync round after reclaim never committed";
				return false;
			}
			std::vector<uint64_t> recorded;
			if (leaveFrame > 0) {
				recorded.push_back(leaveFrame - 1);
			}
			recorded.insert(recorded.end(), hostFrames.begin(), hostFrames.end());
			for (size_t i = 1; i < recorded.size(); ++i) {
				if (recorded[i] != recorded[i - 1] + 1) {
					*error = "resync frame sequence is not contiguous at " + std::to_string(recorded[i - 1]) +
					         " -> " + std::to_string(recorded[i]);
					return false;
				}
			}
			if (resyncHost.IsStopped() || resyncHost.IsFailed()) {
				*error = "resync round stopped instead of committing: " + resyncHost.GetStats().timeoutReason;
				return false;
			}
			return true;
		}

		bool TestHoldHeartbeatsKeepPeersUnadjudicated(std::string* error) {
			const uint16_t port = 43083;
			const uint32_t timeoutMs = 400;
			LoopbackTransport hostT, aT, bT, cT;
			if (!hostT.StartHost(port, error) || !aT.Connect("loopback", port, error) ||
			    !bT.Connect("loopback", port, error) || !cT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = 0x7000000000000083ULL;
				c.timeoutMs = timeoutMs;
				c.localPeerId = local;
				c.peerCount = 4;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			NetLockstepCoordinator host, a, b, c;
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}, {4, 3}}, true), error) ||
			    !a.Start(aT, cfg(2, {{1, 1}}, false), error) ||
			    !b.Start(bT, cfg(3, {{1, 1}}, false), error) ||
			    !c.Start(cT, cfg(4, {{1, 1}}, false), error)) {
				return false;
			}
			uint64_t now = 0;
			auto tickLive = [&](bool tickA, bool tickB, bool tickC) {
				host.Tick(now);
				if (tickA) {
					a.Tick(now);
				}
				if (tickB) {
					b.Tick(now);
				}
				if (tickC) {
					c.Tick(now);
				}
				hostT.AdvanceTimeMs(5);
				if (tickA) {
					aT.AdvanceTimeMs(5);
				}
				if (tickB) {
					bT.AdvanceTimeMs(5);
				}
				if (tickC) {
					cT.AdvanceTimeMs(5);
				}
				now += 5;
			};
			for (; now <= 2000; tickLive(true, true, true)) {
				if (host.IsRunning() && a.IsRunning() && b.IsRunning() && c.IsRunning()) {
					break;
				}
			}
			if (!host.IsRunning()) {
				*error = "heartbeat fixture never started";
				return false;
			}
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !a.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error) ||
				    !b.QueueLocalInput(f, {MakeFrame(300, f + 1)}, {}, error) ||
				    !c.QueueLocalInput(f, {MakeFrame(400, f + 1)}, {}, error)) {
					return false;
				}
			}
			size_t committed = 0;
			NetLockstepReadyFrame ready;
			for (uint64_t guard = 0; guard < 400 && committed < 2; ++guard) {
				tickLive(true, true, true);
				while (host.PopReadyFrame(ready)) {
					++committed;
				}
			}
			if (committed < 2) {
				*error = "heartbeat fixture never committed";
				return false;
			}
			bT.Stop();
			for (uint64_t guard = 0; guard < 400 && host.GetPeerLeaveFrames().count(3) == 0; ++guard) {
				tickLive(true, false, true);
			}
			if (host.GetPeerLeaveFrames().count(3) == 0 || !host.AnyDroppedSeatHeld()) {
				*error = "heartbeat fixture never held the dropped seat";
				return false;
			}
			(void)host.QueueLocalInput(host.GetStats().nextFrame, {MakeFrame(100, host.GetStats().nextFrame + 1)}, {}, error);
			const uint64_t until = now + 4 * timeoutMs;
			while (now <= until) {
				tickLive(false, false, true);
			}
			if (host.GetPeerLeaveFrames().count(4) != 0) {
				*error = "a heartbeating survivor was adjudicated during the pause";
				return false;
			}
			if (host.IsFailed() || host.GetStats().timeoutReason.find("MissingFrameTimeout") != std::string::npos) {
				*error = "the pause fired MissingFrameTimeout: " + host.GetStats().timeoutReason;
				return false;
			}
			if (!host.IsRunning()) {
				*error = "the host left Running during the heartbeat pause";
				return false;
			}
			return true;
		}

		bool TestWaitDoesNotGiveUpDuringHoldPause(std::string* error) {
			const uint16_t port = 43084;
			LoopbackTransport hostT, clientT;
			if (!hostT.StartHost(port, error) || !clientT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = 0x7000000000000084ULL;
				c.timeoutMs = 200;
				c.localPeerId = local;
				c.peerCount = 2;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			NetLockstepCoordinator host, client;
			if (!host.Start(hostT, cfg(1, {{2, 1}}, true), error) || !client.Start(clientT, cfg(2, {{1, 1}}, false), error)) {
				return false;
			}
			uint64_t now = 0;
			for (; now <= 2000; now += 5) {
				host.Tick(now);
				client.Tick(now);
				if (host.IsRunning() && client.IsRunning()) {
					break;
				}
				hostT.AdvanceTimeMs(5);
				clientT.AdvanceTimeMs(5);
			}
			if (!host.IsRunning()) {
				*error = "wait-pause fixture never started";
				return false;
			}
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !client.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error)) {
					return false;
				}
			}
			NetLockstepReadyFrame ready;
			size_t committed = 0;
			for (uint64_t guard = 0; guard < 400 && committed < 2; ++guard, now += 5) {
				host.Tick(now);
				client.Tick(now);
				hostT.AdvanceTimeMs(5);
				clientT.AdvanceTimeMs(5);
				while (host.PopReadyFrame(ready)) {
					++committed;
				}
			}
			clientT.Stop();
			for (uint64_t guard = 0; guard < 80; ++guard, now += 5) {
				host.Tick(now);
				hostT.AdvanceTimeMs(5);
				if (host.AnyDroppedSeatHeld()) {
					break;
				}
			}
			if (!host.AnyDroppedSeatHeld()) {
				*error = "wait-pause fixture never held the drop";
				return false;
			}
			uint32_t pumps = 0;
			ScenarioRunner::SetLockstepCoordinator(&host);
			ScenarioRunner::SetSessionPump([&] {
				++pumps;
				if (pumps == 40) {
					host.ResolveHeldSeat(2, NetLockstepHoldResolution::Expired, now + 5);
				}
			});
			const auto waitStart = std::chrono::steady_clock::now();
			NetLockstepReadyFrame out;
			std::string waitError;
			const bool got = ScenarioRunner::WaitForLockstepControllerFrame(host.GetStats().nextFrame, out, &waitError);
			const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count();
			ScenarioRunner::SetSessionPump(nullptr);
			ScenarioRunner::SetLockstepCoordinator(nullptr);
			if (elapsedMs < 400) {
				*error = "WaitForLockstepControllerFrame gave up during a hold pause after " +
				         std::to_string(elapsedMs) + "ms: " + waitError;
				return false;
			}
			if (got) {
				*error = "the wait produced a frame while the only remote was held";
				return false;
			}
			if (pumps < 40) {
				*error = "the wait never reached the hold resolution pump";
				return false;
			}
			return true;
		}

		bool TestWaitSurvivesHoldAfterPreHoldStall(std::string* error) {
			const uint16_t port = 43092;
			LoopbackTransport hostT, clientT;
			if (!hostT.StartHost(port, error) || !clientT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = 0x7000000000000092ULL;
				c.timeoutMs = 5000;
				c.localPeerId = local;
				c.peerCount = 2;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			NetLockstepCoordinator host, client;
			if (!host.Start(hostT, cfg(1, {{2, 1}}, true), error) || !client.Start(clientT, cfg(2, {{1, 1}}, false), error)) {
				return false;
			}
			uint64_t now = 0;
			for (; now <= 2000; now += 5) {
				host.Tick(now);
				client.Tick(now);
				if (host.IsRunning() && client.IsRunning()) {
					break;
				}
				hostT.AdvanceTimeMs(5);
				clientT.AdvanceTimeMs(5);
			}
			if (!host.IsRunning()) {
				*error = "pre-hold wait fixture never started";
				return false;
			}
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !client.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error)) {
					return false;
				}
			}
			NetLockstepReadyFrame ready;
			size_t committed = 0;
			for (uint64_t guard = 0; guard < 400 && committed < 2; ++guard, now += 5) {
				host.Tick(now);
				client.Tick(now);
				hostT.AdvanceTimeMs(5);
				clientT.AdvanceTimeMs(5);
				while (host.PopReadyFrame(ready)) {
					++committed;
				}
			}
			uint32_t pumps = 0;
			bool dropped = false;
			ScenarioRunner::SetLockstepCoordinator(&host);
			ScenarioRunner::SetSessionPump([&] {
				++pumps;
				now += 15;
				if (!dropped && pumps == 200) {
					clientT.Stop();
					dropped = true;
				}
				if (dropped && host.AnyDroppedSeatHeld() && pumps == 900) {
					host.ResolveHeldSeat(2, NetLockstepHoldResolution::Expired, now);
				}
			});
			const auto waitStart = std::chrono::steady_clock::now();
			NetLockstepReadyFrame out;
			std::string waitError;
			const bool got = ScenarioRunner::WaitForLockstepControllerFrame(host.GetStats().nextFrame, out, &waitError);
			const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count();
			ScenarioRunner::SetSessionPump(nullptr);
			ScenarioRunner::SetLockstepCoordinator(nullptr);
			if (pumps < 900) {
				*error = "WaitForLockstepControllerFrame gave up before the hold resolved after " +
				         std::to_string(elapsedMs) + "ms pumps=" + std::to_string(pumps) + ": " + waitError;
				return false;
			}
			if (got) {
				*error = "the wait produced a frame while the only remote was held";
				return false;
			}
			if (elapsedMs < 10000) {
				*error = "the wait did not cover the pre-hold stall plus the hold";
				return false;
			}
			return true;
		}

		// A reclaimed LIVE seat fences the old transport without a drop, so the pump's resync is the only
		// one and it lands while the host is parked on frames that never come.
		bool TestParkedWaitAppliesPendingResync(std::string* error) {
			const uint16_t port = 43096;
			LoopbackTransport hostT, clientT;
			if (!hostT.StartHost(port, error) || !clientT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = 0x7000000000000096ULL;
				c.timeoutMs = 1000;
				c.localPeerId = local;
				c.peerCount = 2;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			NetLockstepCoordinator host, client;
			if (!host.Start(hostT, cfg(1, {{2, 1}}, true), error) || !client.Start(clientT, cfg(2, {{1, 1}}, false), error)) {
				return false;
			}
			host.DeferStopsToTickBoundary();
			client.DeferStopsToTickBoundary();
			// The wait runs the coordinator on the real clock, so the fixture drives it on that one too.
			auto drive = [&](const std::function<bool()>& done, uint64_t budgetMs) {
				const uint64_t start = NetLockstepNowMs();
				while (true) {
					const uint64_t now = NetLockstepNowMs();
					host.Tick(now);
					client.Tick(now);
					if (done()) {
						return true;
					}
					if (now - start >= budgetMs) {
						return false;
					}
					hostT.AdvanceTimeMs(1);
					clientT.AdvanceTimeMs(1);
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			};
			if (!drive([&] { return host.IsRunning() && client.IsRunning(); }, 3000)) {
				*error = "parked-wait fixture never reached Running";
				return false;
			}
			if (!host.QueueLocalInput(0, {MakeFrame(100, 1)}, {}, error) || !client.QueueLocalInput(0, {MakeFrame(200, 1)}, {}, error)) {
				return false;
			}
			if (!drive([&] { return host.GetStats().framesAccepted == 1; }, 3000)) {
				*error = "parked-wait fixture never committed frame 0";
				return false;
			}
			NetLockstepReadyFrame committed;
			while (host.PopReadyFrame(committed)) {
			}
			host.FinishSimulationTick(0);
			// Tick 1 is queued locally and the remote's frames stop with no drop notice: the fenced reclaim.
			if (!host.QueueLocalInput(1, {MakeFrame(100, 2)}, {}, error)) {
				return false;
			}
			if (host.AnyDroppedSeatHeld()) {
				*error = "the fenced reclaim fixture dropped a seat; the defect needs the no-drop path";
				return false;
			}
			uint32_t pumps = 0;
			ScenarioRunner::SetLockstepCoordinator(&host);
			ScenarioRunner::SetSessionPump([&] {
				++pumps;
				host.RequestResync("player rejoined");
			});
			const auto waitStart = std::chrono::steady_clock::now();
			NetLockstepReadyFrame out;
			std::string waitError;
			const bool got = ScenarioRunner::WaitForLockstepControllerFrame(1, out, &waitError);
			const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count();
			ScenarioRunner::SetSessionPump(nullptr);
			ScenarioRunner::SetLockstepCoordinator(nullptr);
			if (got) {
				*error = "the wait produced a frame the fenced peer never sent";
				return false;
			}
			if (waitError != "ResyncRequested:player rejoined") {
				*error = "the parked wait ended with \"" + waitError + "\" after " + std::to_string(elapsedMs) +
				         "ms instead of the resync the pump scheduled (pumps=" + std::to_string(pumps) + ")";
				return false;
			}
			if (host.HasPendingRecoveryStop()) {
				*error = "the parked wait left the recovery stop pending";
				return false;
			}
			if (host.GetStats().nextFrame != 1 || host.GetStats().framesAccepted != 1) {
				*error = "the parked wait's resync did not land at the tick the sim had not run";
				return false;
			}
			if (elapsedMs >= 500) {
				*error = "the parked wait applied the resync only after " + std::to_string(elapsedMs) + "ms";
				return false;
			}
			if (!drive([&] { return client.IsFailed(); }, 3000) || !client.GetStats().timeoutReason.starts_with("ResyncRequested:")) {
				*error = "the parked wait's resync never reached the client";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS parked_wait_applies_pending_resync frame=1 pumps=" << pumps
			          << " ms=" << elapsedMs << std::endl;
			return true;
		}

		bool TestAnnouncedLeaveStillClosesAtOnce(std::string* error) {
			const uint16_t port = 43085;
			LoopbackTransport hostT, leaverT, stayerT;
			if (!hostT.StartHost(port, error) || !leaverT.Connect("loopback", port, error) || !stayerT.Connect("loopback", port, error)) {
				return false;
			}
			auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
				NetLockstepConfig c;
				c.sessionId = 0x7000000000000085ULL;
				c.timeoutMs = 5000;
				c.localPeerId = local;
				c.peerCount = 3;
				c.remoteTransportPeerIds = std::move(transports);
				c.relayToOtherPeers = relay;
				c.scenario = "LockstepSelfTest";
				c.ownershipPolicy = "unique-id-split";
				return c;
			};
			NetLockstepCoordinator host, leaver, stayer;
			if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error) ||
			    !leaver.Start(leaverT, cfg(2, {{1, 1}}, false), error) ||
			    !stayer.Start(stayerT, cfg(3, {{1, 1}}, false), error)) {
				return false;
			}
			uint64_t now = 0;
			auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
				for (const uint64_t until = now + forMs; now <= until; now += 5) {
					host.Tick(now);
					leaver.Tick(now);
					stayer.Tick(now);
					if (done()) {
						return true;
					}
					hostT.AdvanceTimeMs(5);
					leaverT.AdvanceTimeMs(5);
					stayerT.AdvanceTimeMs(5);
				}
				return false;
			};
			if (!drive(2000, [&] { return host.IsRunning() && leaver.IsRunning() && stayer.IsRunning(); })) {
				*error = "announced-leave control never started";
				return false;
			}
			for (uint64_t f = 0; f < 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !leaver.QueueLocalInput(f, {MakeFrame(200, f + 1)}, {}, error) ||
				    !stayer.QueueLocalInput(f, {MakeFrame(300, f + 1)}, {}, error)) {
					return false;
				}
			}
			NetLockstepReadyFrame ready;
			size_t committed = 0;
			if (!drive(2000, [&] {
					while (host.PopReadyFrame(ready)) {
						++committed;
					}
					return committed >= 2;
				})) {
				*error = "announced-leave control never committed";
				return false;
			}
			leaver.Leave("bye");
			if (!drive(2000, [&] { return host.GetPeerLeaveFrames().count(2) != 0 && stayer.GetPeerLeaveFrames().count(2) != 0; })) {
				*error = "the announced leave never reached both survivors";
				return false;
			}
			if (host.AnyDroppedSeatHeld() || stayer.AnyDroppedSeatHeld()) {
				*error = "an announced leave paused the match";
				return false;
			}
			const uint64_t leaveFrame = host.GetPeerLeaveFrames().at(2);
			for (uint64_t f = leaveFrame; f < leaveFrame + 2; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100, f + 1)}, {}, error) ||
				    !stayer.QueueLocalInput(f, {MakeFrame(300, f + 1)}, {}, error)) {
					return false;
				}
			}
			size_t extra = 0;
			if (!drive(2000, [&] {
					while (host.PopReadyFrame(ready)) {
						++extra;
					}
					return extra >= 2;
				})) {
				*error = "survivors did not advance after an announced leave";
				return false;
			}
			return true;
		}

		NetValueObservation MakeValueObservation(uint8_t sender, uint64_t objectUID, uint64_t tick, uint32_t ordinal, const std::string& key, double number) {
			NetValueObservation observation;
			observation.senderPeerId = sender;
			observation.objectUID = objectUID;
			observation.tick = tick;
			observation.ordinal = ordinal;
			observation.mapKind = 0;
			observation.key = key;
			observation.op = 0;
			observation.numberValue = number;
			return observation;
		}

		bool TestValueObservationCodec(std::string* error) {
			NetLockstepFrame frame;
			frame.senderPeerId = 1;
			frame.targetFrame = 4;
			frame.roundId = 0x14000000000000A0ULL;
			frame.frames = {MakeFrame(100, 1)};
			frame.valueObservations = {
				MakeValueObservation(1, 1048653, 12, 1, "AI_StuckForTime", 3749.85),
				MakeValueObservation(1, 1048653, 12, 2, "AI_StuckForTime", 0),
			};
			frame.valueObservations[1].op = 1;
			frame.valueObservations[1].numberValue = 0;
			NetValueObservation text;
			text.senderPeerId = 1;
			text.objectUID = 9;
			text.tick = 3;
			text.ordinal = 3;
			text.mapKind = 1;
			text.key = "name";
			text.op = 0;
			text.stringValue = "ok";
			frame.valueObservations.push_back(text);
			if (!RoundTrip({frame}, error)) {
				return false;
			}
			std::vector<uint8_t> bytes;
			if (!EncodePacket({frame}, bytes, error)) {
				return false;
			}
			const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(bytes);
			const NetLockstepFrame* got = decoded.ok ? std::get_if<NetLockstepFrame>(&decoded.packet.payload) : nullptr;
			if (!got || got->valueObservations != frame.valueObservations) {
				*error = "value observation codec lost a field";
				return false;
			}
			// A script may store any double; the wire carries the exact bits so every peer commits the same value.
			NetLockstepFrame specialFrame = frame;
			specialFrame.valueObservations = {
			    MakeValueObservation(1, 1, 1, 1, "nan", std::numeric_limits<double>::quiet_NaN()),
			    MakeValueObservation(1, 1, 1, 2, "inf", std::numeric_limits<double>::infinity()),
			    MakeValueObservation(1, 1, 1, 3, "ninf", -std::numeric_limits<double>::infinity()),
			    MakeValueObservation(1, 1, 1, 4, "nzero", -0.0),
			};
			std::vector<uint8_t> specialBytes;
			NetLockstepError encodeError;
			if (!NetLockstepCodec::Encode({specialFrame}, specialBytes, &encodeError)) {
				*error = "non-finite number value observations did not encode: " + encodeError.message;
				return false;
			}
			const NetLockstepDecodeResult specialDecoded = NetLockstepCodec::Decode(specialBytes);
			const NetLockstepFrame* specialGot = specialDecoded.ok ? std::get_if<NetLockstepFrame>(&specialDecoded.packet.payload) : nullptr;
			if (!specialGot || specialGot->valueObservations.size() != specialFrame.valueObservations.size()) {
				*error = "non-finite number value observations did not decode";
				return false;
			}
			for (size_t i = 0; i < specialFrame.valueObservations.size(); ++i) {
				uint64_t sent = 0;
				uint64_t got = 0;
				std::memcpy(&sent, &specialFrame.valueObservations[i].numberValue, sizeof(sent));
				std::memcpy(&got, &specialGot->valueObservations[i].numberValue, sizeof(got));
				if (sent != got) {
					*error = "number value observation " + specialFrame.valueObservations[i].key + " did not round-trip bit-exactly";
					return false;
				}
			}
			std::vector<uint8_t> truncated;
			if (!EncodePacket({frame}, truncated, error)) {
				return false;
			}
			truncated.pop_back();
			const uint32_t payloadLength = static_cast<uint32_t>(truncated.size() - NetLockstepCodec::c_HeaderBytes);
			for (int i = 0; i < 4; ++i) {
				truncated[12 + i] = static_cast<uint8_t>(payloadLength >> (i * 8));
			}
			if (!ExpectDecodeError(truncated, NetLockstepErrorCode::TruncatedPayload, error)) {
				return false;
			}
			NetLockstepFrame longKey = frame;
			longKey.valueObservations = {MakeValueObservation(1, 1, 1, 1, std::string(NetLockstepCodec::c_MaxValueKeyBytes + 1, 'k'), 1)};
			if (NetLockstepCodec::Encode({longKey}, bytes, &encodeError) || encodeError.code != NetLockstepErrorCode::StringTooLong) {
				*error = "an oversized value key was not StringTooLong";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS value_observation_codec n=" << frame.valueObservations.size() << std::endl;
			return true;
		}

		bool TestValueObservationV19StillDecodes(std::string* error) {
			NetLockstepFrame frame;
			frame.senderPeerId = 1;
			frame.targetFrame = 4;
			frame.roundId = 0x14000000000000A0ULL;
			frame.frames = {MakeFrame(100, 1)};
			std::vector<uint8_t> bytes;
			if (!EncodePacket({frame}, bytes, error) || bytes.size() < 6) {
				return false;
			}
			bytes[4] = static_cast<uint8_t>(NetLockstepCodec::c_HoldResolutionVersion);
			bytes[5] = 0;
			const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(bytes);
			const NetLockstepFrame* got = decoded.ok ? std::get_if<NetLockstepFrame>(&decoded.packet.payload) : nullptr;
			if (!got || !got->valueObservations.empty() || got->targetFrame != frame.targetFrame) {
				*error = "a version-19 frame with no value section did not decode";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS value_observation_v19_still_decodes" << std::endl;
			return true;
		}

		bool TestValueObservationRelay(std::string* error) {
			LoopbackTransport hostTransport, clientTransport;
			NetLockstepCoordinator host, client;
			const uint16_t delay = 3;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, 0x70000000000000A1ULL, delay, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, 0x70000000000000A1ULL, delay, NetTransportLane::ControlReliable);
			hostConfig.roundId = 0x14000000000000A1ULL;
			hostConfig.timeoutMs = 4000;
			clientConfig.timeoutMs = 4000;
			if (!StartCoordinatorPair(43121, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error, 8000)) {
				return false;
			}
			const uint64_t uid = 1048654;
			const uint64_t produced = 5;
			const uint64_t target = produced + delay;
			const NetValueObservation write = MakeValueObservation(1, uid, produced, 1, "AI_StuckForTime", 12.5);
			const uint64_t frameCount = 12;
			for (uint64_t f = 0; f < frameCount; ++f) {
				std::vector<NetValueObservation> values;
				if (f == produced) {
					values.push_back(write);
				}
				if (!host.QueueLocalInput(f, {MakeFrame(100 + static_cast<int64_t>(f), f + 1)}, {}, error, {}, values) ||
				    !client.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error)) {
					return false;
				}
			}
			std::map<uint64_t, std::vector<NetValueObservation>> hostByFrame;
			std::map<uint64_t, std::vector<NetValueObservation>> clientByFrame;
			size_t hostReady = 0, clientReady = 0;
			auto collect = [&]() {
				NetLockstepReadyFrame ready;
				while (host.PopReadyFrame(ready)) {
					std::vector<NetValueObservation> all = ready.localValueObservations;
					all.insert(all.end(), ready.remoteValueObservations.begin(), ready.remoteValueObservations.end());
					hostByFrame[ready.frame] = std::move(all);
					++hostReady;
				}
				while (client.PopReadyFrame(ready)) {
					std::vector<NetValueObservation> all = ready.localValueObservations;
					all.insert(all.end(), ready.remoteValueObservations.begin(), ready.remoteValueObservations.end());
					clientByFrame[ready.frame] = std::move(all);
					++clientReady;
				}
			};
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] {
					collect();
					return hostReady >= frameCount && clientReady >= frameCount;
				}, error, 16000)) {
				return false;
			}
			std::map<std::string, double> hostMap;
			std::map<std::string, double> clientMap;
			auto apply = [](std::map<std::string, double>& numbers, const std::vector<NetValueObservation>& observations) {
				for (const NetValueObservation& observation: observations) {
					MovableObject::PendingValueOp op;
					op.objectUID = observation.objectUID;
					op.map = static_cast<MovableObject::ValueMapKind>(observation.mapKind);
					op.op = static_cast<MovableObject::ValueMapOp>(observation.op);
					op.key = observation.key;
					op.number = observation.numberValue;
					op.ordinal = observation.ordinal;
					if (op.map != MovableObject::ValueMapKind::Number) {
						continue;
					}
					if (op.op == MovableObject::ValueMapOp::Remove) {
						numbers.erase(op.key);
					} else {
						numbers[op.key] = op.number;
					}
				}
			};
			for (uint64_t f = delay; f < target; ++f) {
				if (!hostByFrame[f].empty() || !clientByFrame[f].empty()) {
					*error = "a value observation landed before frame N+D";
					return false;
				}
			}
			if (hostByFrame[target] != clientByFrame[target] || hostByFrame[target].size() != 1 ||
			    hostByFrame[target][0].key != write.key || hostByFrame[target][0].numberValue != write.numberValue) {
				*error = "peers did not commit the same value observation at N+D";
				return false;
			}
			apply(hostMap, hostByFrame[target]);
			apply(clientMap, clientByFrame[target]);
			if (hostMap != clientMap || hostMap.size() != 1 || hostMap["AI_StuckForTime"] != 12.5) {
				*error = "settled number maps differed at N+D";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS value_observation_relay delay=" << delay << " target=" << target << std::endl;
			return true;
		}

		bool TestValueObservationReplay(std::string* error) {
			LoopbackTransport hostTransport, clientTransport;
			NetLockstepCoordinator host, client;
			const uint16_t delay = 3;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, 0x70000000000000A4ULL, delay, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, 0x70000000000000A4ULL, delay, NetTransportLane::ControlReliable);
			hostConfig.roundId = 0x14000000000000A4ULL;
			hostConfig.timeoutMs = 4000;
			clientConfig.timeoutMs = 4000;
			if (!StartCoordinatorPair(43124, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error, 8000)) {
				return false;
			}
			const uint64_t uid = 1048654;
			const uint64_t produced = 5;
			const uint64_t target = produced + delay;
			const NetValueObservation write = MakeValueObservation(1, uid, produced, 1, "AI_StuckForTime", 12.5);
			const uint64_t frameCount = 12;
			for (uint64_t f = 0; f < frameCount; ++f) {
				std::vector<NetValueObservation> values;
				if (f == produced) {
					values.push_back(write);
				}
				if (!host.QueueLocalInput(f, {MakeFrame(100 + static_cast<int64_t>(f), f + 1)}, {}, error, {}, values) ||
				    !client.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error)) {
					return false;
				}
			}
			std::map<uint64_t, std::vector<NetValueObservation>> liveByFrame;
			size_t hostReady = 0, clientReady = 0;
			auto collect = [&]() {
				NetLockstepReadyFrame ready;
				while (host.PopReadyFrame(ready)) {
					std::vector<NetValueObservation> all = ready.localValueObservations;
					all.insert(all.end(), ready.remoteValueObservations.begin(), ready.remoteValueObservations.end());
					liveByFrame[ready.frame] = std::move(all);
					++hostReady;
				}
				while (client.PopReadyFrame(ready)) {
					++clientReady;
				}
			};
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] {
					collect();
					return hostReady >= frameCount && clientReady >= frameCount;
				}, error, 16000)) {
				return false;
			}
			std::map<std::string, double> liveMap;
			for (const NetValueObservation& observation: liveByFrame[target]) {
				if (observation.op == 0 && observation.mapKind == 0) {
					liveMap[observation.key] = observation.numberValue;
				}
			}
			if (liveMap.size() != 1 || liveMap["AI_StuckForTime"] != 12.5) {
				*error = "the live settle frame did not hold peer A's NumberValue";
				return false;
			}
			const auto directory = std::filesystem::temp_directory_path() / ("cc-value-replay-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::error_code created;
			std::filesystem::create_directories(directory, created);
			if (created) {
				*error = "could not create value-replay test directory";
				return false;
			}
			const auto path = directory / "match.ccreplay";
			struct Cleanup {
				std::filesystem::path path;
				~Cleanup() {
					std::error_code ignored;
					std::filesystem::remove(path, ignored);
					std::filesystem::remove(path.parent_path(), ignored);
				}
			} cleanup{path};
			const NetMatchConfig replayConfig = NetMatchConfigUtil::MakeDefault(0x50484134564C5231ULL);
			NetMatchReplayWriter writer;
			if (!writer.Open(path.string(), replayConfig, error)) {
				return false;
			}
			if (!writer.WriteFrame(target - 1, {MakeFrame(100, target)}, {}, {}, {}, error)) {
				return false;
			}
			if (!writer.WriteFrame(target, {MakeFrame(100, target + 1)}, {}, {}, liveByFrame[target], error)) {
				return false;
			}
			writer.Close();
			NetMatchReplayReader reader;
			if (!reader.Open(path.string(), error)) {
				return false;
			}
			NetLockstepFrame recorded;
			bool eof = false;
			if (!reader.ReadFrame(recorded, eof, error) || !recorded.valueObservations.empty()) {
				*error = "a recorded frame with no value observations did not stay empty";
				return false;
			}
			if (!reader.ReadFrame(recorded, eof, error) || recorded.targetFrame != target ||
			    recorded.valueObservations != liveByFrame[target] || recorded.valueObservations.size() != 1 ||
			    recorded.valueObservations[0].senderPeerId != 1) {
				*error = "replay lost the settled NumberValue";
				return false;
			}
			std::map<std::string, double> replayedMap;
			for (const NetValueObservation& observation: recorded.valueObservations) {
				if (observation.op == 0 && observation.mapKind == 0) {
					replayedMap[observation.key] = observation.numberValue;
				}
			}
			if (replayedMap != liveMap) {
				*error = "replayed number maps did not equal the live settle maps";
				return false;
			}
			reader.Close();
			std::cout << "[net-lockstep-selftest] PASS value_observation_replay target=" << target << std::endl;
			return true;
		}

		bool TestReplayPlayerBindings(std::string* error) {
			LoopbackTransport hostTransport, clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, 0x70000000000000A5ULL, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, 0x70000000000000A5ULL, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = 0x14000000000000A5ULL;
			hostConfig.timeoutMs = 4000;
			clientConfig.timeoutMs = 4000;
			if (!StartCoordinatorPair(43125, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error, 8000)) {
				return false;
			}
			NetGamePlayerBindings hostBinding;
			hostBinding.players[0].active = true;
			hostBinding.players[0].team = 0;
			hostBinding.players[0].controlledUID = 100;
			NetGamePlayerBindings clientBinding;
			clientBinding.players[0].active = true;
			clientBinding.players[0].team = 1;
			clientBinding.players[0].controlledUID = 200;
			const std::vector<NetGameCommand> hostCommands{{1, hostBinding}};
			const std::vector<NetGameCommand> clientCommands{{2, clientBinding}};
			if (!host.QueueLocalInput(0, {MakeFrame(100, 1)}, hostCommands, error) ||
			    !client.QueueLocalInput(0, {MakeFrame(200, 1)}, clientCommands, error)) {
				return false;
			}
			std::vector<NetGameCommand> liveCommands;
			std::vector<ControllerFrame> liveFrames;
			size_t hostReady = 0, clientReady = 0;
			auto collect = [&]() {
				NetLockstepReadyFrame ready;
				while (host.PopReadyFrame(ready)) {
					if (ready.frame == 0) {
						liveFrames = ready.localFrames;
						liveFrames.insert(liveFrames.end(), ready.remoteFrames.begin(), ready.remoteFrames.end());
						std::sort(liveFrames.begin(), liveFrames.end(), [](const ControllerFrame& lhs, const ControllerFrame& rhs) {
							return lhs.actorUniqueID < rhs.actorUniqueID;
						});
						liveCommands = ready.localCommands;
						liveCommands.insert(liveCommands.end(), ready.remoteCommands.begin(), ready.remoteCommands.end());
					}
					++hostReady;
				}
				while (client.PopReadyFrame(ready)) {
					++clientReady;
				}
			};
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] {
					collect();
					return hostReady >= 1 && clientReady >= 1;
				}, error, 8000)) {
				return false;
			}
			std::map<uint8_t, NetGamePlayerBindings> liveBindings;
			for (const NetGameCommand& command: liveCommands) {
				if (const auto* bindings = std::get_if<NetGamePlayerBindings>(&command.payload)) {
					liveBindings[command.senderPeerId] = *bindings;
				}
			}
			if (liveBindings.size() != 2 || liveBindings[1] != hostBinding || liveBindings[2] != clientBinding) {
				*error = "the live ready frame did not keep both peers' bindings";
				return false;
			}
			const auto directory = std::filesystem::temp_directory_path() / ("cc-binding-replay-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::error_code created;
			std::filesystem::create_directories(directory, created);
			if (created) {
				*error = "could not create binding-replay test directory";
				return false;
			}
			const auto path = directory / "match.ccreplay";
			struct Cleanup {
				std::filesystem::path path;
				~Cleanup() {
					std::error_code ignored;
					std::filesystem::remove(path, ignored);
					std::filesystem::remove(path.parent_path(), ignored);
				}
			} cleanup{path};
			const NetMatchConfig replayConfig = NetMatchConfigUtil::MakeDefault(0x50484134424E4431ULL);
			NetMatchReplayWriter writer;
			if (!writer.Open(path.string(), replayConfig, error)) {
				return false;
			}
			if (!writer.WriteFrame(0, liveFrames, liveCommands, {}, {}, error)) {
				return false;
			}
			writer.Close();
			NetMatchReplayReader reader;
			if (!reader.Open(path.string(), error)) {
				return false;
			}
			NetLockstepFrame recorded;
			bool eof = false;
			if (!reader.ReadFrame(recorded, eof, error) || recorded.targetFrame != 0) {
				*error = "the two-binding record did not read back";
				return false;
			}
			std::map<uint8_t, NetGamePlayerBindings> replayedBindings;
			for (const NetGameCommand& command: recorded.commands) {
				if (const auto* bindings = std::get_if<NetGamePlayerBindings>(&command.payload)) {
					replayedBindings[command.senderPeerId] = *bindings;
				}
			}
			if (replayedBindings != liveBindings) {
				*error = "replayed bindings per sender did not equal the live ones";
				return false;
			}
			reader.Close();
			std::cout << "[net-lockstep-selftest] PASS replay_player_bindings senders=2" << std::endl;
			return true;
		}

		bool TestValueObservationNonOwnerDropped(std::string* error) {
			LoopbackTransport hostTransport, clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, 0x70000000000000A2ULL, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, 0x70000000000000A2ULL, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = 0x14000000000000A2ULL;
			hostConfig.timeoutMs = 4000;
			hostConfig.matchConfig.hostPeerId = 1;
			clientConfig.timeoutMs = 4000;
			clientConfig.matchConfig.hostPeerId = 1;
			if (!StartCoordinatorPair(43122, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error, 8000)) {
				return false;
			}
			struct CoordinatorGuard {
				explicit CoordinatorGuard(NetLockstepCoordinator* coordinator) { ScenarioRunner::SetLockstepCoordinator(coordinator); }
				~CoordinatorGuard() { ScenarioRunner::SetLockstepCoordinator(nullptr); }
			} guard(&host);
			const uint64_t evenUID = 1048654;
			if (host.ResolveActorOwner(static_cast<int64_t>(evenUID), 0, true) != 1) {
				*error = "unique-id-split did not give the even UID to peer 1";
				return false;
			}
			if (g_MovableMan.ValueObservationAuthority(0) != 1 || g_MovableMan.ValueObservationAuthority(evenUID) != 1) {
				*error = "a value observation without an actor root was not hosted";
				return false;
			}
			const uint64_t rejectedBefore = g_MovableMan.GetValueObservationsRejected();
			const NetValueObservation stranger = MakeValueObservation(2, evenUID, 1, 1, "AI_StuckForTime", 99);
			g_MovableMan.CommitValueObservations(0, {}, {stranger});
			if (g_MovableMan.GetValueObservationsRejected() != rejectedBefore + 1) {
				*error = "a non-owner value observation was not dropped";
				return false;
			}
			const NetValueObservation owned = MakeValueObservation(1, evenUID, 1, 1, "AI_StuckForTime", 3);
			g_MovableMan.CommitValueObservations(0, {owned}, {});
			if (g_MovableMan.GetValueObservationsRejected() != rejectedBefore + 1) {
				*error = "the owner value observation was counted as rejected";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS value_observation_non_owner_dropped rejected=" << (rejectedBefore + 1) << std::endl;
			return true;
		}

		bool TestValueObservationOverflowCarry(std::string* error) {
			LoopbackTransport hostTransport, clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig = MakeCoordinatorConfig(1, 2, 0x70000000000000A3ULL, 0, NetTransportLane::ControlReliable);
			NetLockstepConfig clientConfig = MakeCoordinatorConfig(2, 1, 0x70000000000000A3ULL, 0, NetTransportLane::ControlReliable);
			hostConfig.roundId = 0x14000000000000A3ULL;
			hostConfig.timeoutMs = 4000;
			clientConfig.timeoutMs = 4000;
			if (!StartCoordinatorPair(43123, hostTransport, clientTransport, host, client, hostConfig, clientConfig, error)) {
				return false;
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] { return host.IsRunning() && client.IsRunning(); }, error, 8000)) {
				return false;
			}
			uint64_t nextObject = 40000000;
			const auto flood = [&](size_t count, uint64_t tick) {
				std::vector<NetValueObservation> observations;
				for (size_t i = 0; i < count; ++i) {
					observations.push_back(MakeValueObservation(1, nextObject++, tick, static_cast<uint32_t>(1 + i), std::string(80, 'k') + std::to_string(i), static_cast<double>(i)));
				}
				return observations;
			};
			const size_t burst = 2048;
			const size_t frameCount = 10;
			std::vector<NetValueObservation> sent;
			for (uint64_t f = 0; f < frameCount; ++f) {
				std::vector<NetValueObservation> observations = f < 2 ? flood(burst, 900 + f) : std::vector<NetValueObservation>{};
				sent.insert(sent.end(), observations.begin(), observations.end());
				if (!host.QueueLocalInput(f, {MakeFrame(100 + static_cast<int64_t>(f), f + 1)}, {}, error, {}, observations) ||
				    !client.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error)) {
					return false;
				}
			}
			std::vector<NetValueObservation> hostSeen, clientSeen;
			size_t hostReady = 0, clientReady = 0;
			auto collect = [&](NetLockstepCoordinator& coordinator, std::vector<NetValueObservation>& into, size_t& count) {
				NetLockstepReadyFrame ready;
				while (coordinator.PopReadyFrame(ready)) {
					into.insert(into.end(), ready.localValueObservations.begin(), ready.localValueObservations.end());
					into.insert(into.end(), ready.remoteValueObservations.begin(), ready.remoteValueObservations.end());
					++count;
				}
			};
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] {
					collect(host, hostSeen, hostReady);
					collect(client, clientSeen, clientReady);
					return hostReady >= frameCount && clientReady >= frameCount;
				}, error, 16000)) {
				return false;
			}
			if (host.GetStats().valueObservationsCarried == 0) {
				*error = "a frame of new value writes did not carry anything over";
				return false;
			}
			if (host.GetStats().valueObservationsDropped != 0) {
				*error = "a burst the quiet frames could drain still dropped value writes";
				return false;
			}
			if (hostSeen != clientSeen || hostSeen != sent) {
				*error = "the carried value observations did not all arrive in their sampled order: " +
				         std::to_string(hostSeen.size()) + " of " + std::to_string(sent.size());
				return false;
			}
			const uint64_t carriedInBurst = host.GetStats().valueObservationsCarried;
			for (uint64_t f = frameCount; f < frameCount + 8; ++f) {
				if (!host.QueueLocalInput(f, {MakeFrame(100 + static_cast<int64_t>(f), f + 1)}, {}, error, {}, flood(NetLockstepCodec::c_MaxObservationsPerPacket, 1000 + f)) ||
				    !client.QueueLocalInput(f, {MakeFrame(200 + static_cast<int64_t>(f), f + 1)}, {}, error)) {
					return false;
				}
			}
			if (!DriveCoordinators(hostTransport, clientTransport, host, client, [&] {
					collect(host, hostSeen, hostReady);
					collect(client, clientSeen, clientReady);
					return hostReady >= frameCount + 8 && clientReady >= frameCount + 8;
				}, error, 16000)) {
				return false;
			}
			if (host.GetStats().valueObservationsDropped == 0) {
				*error = "the held value-observation set was not bounded under a sustained flood";
				return false;
			}
			const std::vector<NetValueObservation> handedBack = host.TakeDroppedValueObservations();
			if (handedBack.size() != host.GetStats().valueObservationsDropped) {
				*error = "the dropped value writes were not all handed back";
				return false;
			}
			if (hostSeen != clientSeen) {
				*error = "a bounded held set left the two peers with different value observations";
				return false;
			}
			std::cout << "[net-lockstep-selftest] PASS value_observation_overflow_carry burst=" << burst << " carried=" << carriedInBurst
			          << " sustained_dropped=" << host.GetStats().valueObservationsDropped << std::endl;
			return true;
		}
	}

	int NetLockstepSelfTest::Run() {
		if (!TimerMan::IsConstructed()) TimerMan::Construct();
		if (!MovableMan::IsConstructed()) MovableMan::Construct();
		if (!ActivityMan::IsConstructed()) ActivityMan::Construct();
		if (!AudioMan::IsConstructed()) AudioMan::Construct();
		auto fail = [](const std::string& message) {
			std::cerr << "[net-lockstep-selftest] FAIL: " << message << std::endl;
			return 1;
		};

		std::string error;
		if (!TestRoundTrips(&error) ||
		    !TestSnapshotConstructionKeepsPendingCommands(&error) ||
		    !TestSenderDropsUncontrolledTeamCommands(&error) ||
		    !TestCoordinatorOwedFrameRetryEndsWithTheRound(&error) ||
		    !TestCoordinatorOwedFrameKeepsItsCommands(&error) ||
		    !TestCoordinatorStoppedRoundStopsResending(&error) ||
		    !TestCoordinatorFailedRoundStopsResending(&error) ||
		    !TestCoordinatorOwedFrameOutlivesItsLocalCommit(&error) ||
		    !TestCoordinatorReadoptResendSurvivesARefusedSend(&error) ||
		    !TestReviewAuthorityStartDoesNotFailTheRound(&error) ||
		    !TestReviewReadoptClearsTheLeftPeer(&error) ||
		    !TestReviewReadoptClearsTheDeferredStop(&error) ||
		    !TestReviewFourPeerStrayStartRate(&error) ||
		    !TestCoordinatorClientFollowsTheHostsNewRound(&error) ||
		    !TestCoordinatorRestartedPeerIsNotHandedTheOldRound(&error) ||
		    !TestCoordinatorRepeatedStartCarriesTheRound(&error) ||
		    !TestCoordinatorRepeatedStartsDoNotAmplify(&error) ||
		    !TestObservationSlotCodec(&error) ||
		    !TestCanonicalHeader(&error) ||
		    !TestDecodeFailures(&error) ||
		    !TestSemanticFailures(&error) ||
		    !TestRecoveryStopsAtCompletedTick(&error) ||
		    !TestCompletionDrainsAppliedTicks(&error) ||
		    !TestCoordinatorDelayedHappyPath(&error) ||
		    !TestCoordinatorFrameBeforeStart(&error) ||
		    !TestCoordinatorIgnoresStaleRound(&error) ||
		    !TestCoordinatorPerSenderDelay(&error) ||
		    !TestCoordinatorPerSenderDelayMismatch(&error) ||
		    !TestCoordinatorIgnoresSessionPacketsAtHandoff(&error) ||
		    !TestCoordinatorUnreliableOutOfOrderDuplicate(&error) ||
		    !TestCoordinatorMissingFrameTimeout(&error) ||
		    !TestActivityGateAgreesAcrossPeers(&error) ||
		    !TestB2SeatSnapshotCodec(&error) ||
		    !TestRecoveryWireRefusals(&error) ||
		    !TestRecoveryWireRelayRetry(&error) ||
		    !TestRecoveryInputMembership(&error) ||
		    !TestB2SeatSnapshotRelay(&error) ||
		    !TestB2SeatSnapshotAuthority(&error) ||
		    !TestB2SeatSnapshotReplayAndPublication(&error) ||
		    !TestB2SeatSnapshotInitiallyComplete(&error) ||
		    !TestB2SeatSnapshotCoalesces(&error) ||
		    !TestB2SeatSnapshotResyncSeedsNewPeer(&error) ||
		    !TestB2SeatSnapshotDoesNotReviveDepartedTransport(&error) ||
		    !TestAnnouncedLeaveHoldsNothing(&error) ||
		    !TestHoldPauseCommitsNothing(&error) ||
		    !TestHoldExpiredResumesWithoutSeat(&error) ||
		    !TestHoldReclaimedResyncsAtLeaveFrame(&error) ||
		    !TestTwoPeerReclaimedRequestsResync(&error) ||
		    !TestTwoPeerSubstitutedRequestsResync(&error) ||
		    !TestTwoPeerExpiredEndsLastPlayer(&error) ||
		    !TestThreePeerReclaimedRequestsResync(&error) ||
		    !TestResyncRoundCommitsAfterReclaimed(&error) ||
		    !TestHoldHeartbeatsKeepPeersUnadjudicated(&error) ||
		    !TestWaitDoesNotGiveUpDuringHoldPause(&error) ||
		    !TestWaitSurvivesHoldAfterPreHoldStall(&error) ||
		    !TestParkedWaitAppliesPendingResync(&error) ||
		    !TestAnnouncedLeaveStillClosesAtOnce(&error) ||
		    !TestCoordinatorHeldSeatWithASurvivor(&error) ||
		    !TestCoordinatorThreePeer(&error) ||
		    !TestCoordinatorSeatlessRelayHost(&error) ||
		    !TestCoordinatorRejectsUnboundPackets(&error) ||
		    !TestCoordinatorPeerLeave(&error) ||
		    !TestCoordinatorHostAdjudicatesSilentPeer(&error) ||
		    !TestCoordinatorSilentHostStillTimesOut(&error) ||
		    !TestCoordinatorRelayBacklogHeals(&error) ||
		    !TestCoordinatorRelayFailureDropsPeer(&error) ||
		    !TestCoordinatorDroppedSeatHold(&error) ||
		    !TestCoordinatorHeldSeatKeepsPlaying(&error) ||
		    !TestSeatStateNeverReadUnderTheServiceLock(&error) ||
		    !TestSeatStateNeverReadUnderTheServiceLock(&error, true) ||
		    !TestHeldSeatOwnershipAgreesAcrossPeers(&error) ||
		    !TestSessionPumpRunsWhileTheRoundWaits(&error) ||
		    !TestCoordinatorAdjudicatedPeerKeepsItsSeat(&error) ||
		    !TestRelayHostFinishesWhatItOwes(&error) ||
		    !TestObservationOverflowCarry(&error) ||
		    !TestValueObservationCodec(&error) ||
		    !TestValueObservationV19StillDecodes(&error) ||
		    !TestValueObservationRelay(&error) ||
		    !TestValueObservationReplay(&error) ||
		    !TestReplayPlayerBindings(&error) ||
		    !TestValueObservationNonOwnerDropped(&error) ||
		    !TestValueObservationOverflowCarry(&error) ||
		    !TestStaleRoundFrameStillCountsAsTraffic(&error) ||
		    !TestObservationFaultsAreToldApart(&error) ||
		    !TestStaleRoundFrameLeavesTheLiveTable(&error) ||
		    !TestResyncStragglersAreNotHoles(&error) ||
		    !TestRefusedBlockLeavesNoBindings(&error) ||
		    !TestFourPeerObservationRelayBytes(&error) ||
		    !TestCongestedRelayHoldsEveryPeer(&error) ||
		    !TestFourPeerRoundRunsToLength(&error) ||
		    !TestDeadLinkLosesOnlyItsOwnSeat(&error) ||
		    !TestDeadLinkHealedInTimeKeepsEverySeat(&error)) {
			return fail(error);
		}

		if (NetResyncSelfTest::Run() != 0 || NetResyncRuntimeSelfTest::Run() != 0) return fail("resync regression suite failed");
		std::cout << "[net-lockstep-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
