#include "NetResyncRuntimeSelfTest.h"

#include "LoopbackTransport.h"
#include "NetLockstep.h"
#include "NetResyncState.h"
#include "System/ScenarioRunner.h"

#include <algorithm>
#include <array>
#include <bit>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace RTE {

	namespace {
		constexpr uint64_t c_Start = 41;
		using Batches = std::vector<std::vector<NetGameCommand>>;
		using CommandsByFrame = std::map<uint64_t, std::vector<NetGameCommand>>;
		using InputsByFrame = std::map<uint64_t, NetLockstepFrame>;

		bool Check(bool condition, std::string* error, const std::string& message) {
			if (!condition) *error = message;
			return condition;
		}

		NetGameCommand Command(uint8_t peer, uint64_t sequence) {
			return {peer, NetGameSetTeamFunds{0, static_cast<int32_t>(peer * 1000 + sequence)}, sequence};
		}

		ControllerFrame Input(uint8_t peer, uint64_t target) {
			ControllerFrame frame;
			frame.actorUniqueID = 100 + peer;
			frame.analogMoveX = static_cast<int16_t>(target);
			return frame;
		}

		struct RecoveryTransport : LoopbackTransport {
			bool holdLaterChunks = false;
			size_t recoveryAttempts = 0, refusedChunks = 0;
			std::vector<uint8_t> firstRefused;
			bool resumedExactChunk = false, checkedResume = false;

			bool Send(NetPeerId peer, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error = nullptr, bool* congested = nullptr) override {
				const auto packet = NetLockstepCodec::Decode(bytes);
				if (packet.ok) if (const auto* chunk = std::get_if<NetLockstepRecoveryChunk>(&packet.packet.payload)) {
					++recoveryAttempts;
					if (holdLaterChunks && chunk->offset != 0) {
						if (firstRefused.empty()) firstRefused = bytes;
						++refusedChunks;
						if (error) *error = "recovery chunk queue is full";
						if (congested) *congested = true;
						return false;
					}
					if (!holdLaterChunks && !firstRefused.empty() && !checkedResume) { resumedExactChunk = bytes == firstRefused; checkedResume = true; }
				}
				return LoopbackTransport::Send(peer, lane, bytes, error, congested);
			}
		};

		struct Pair {
			RecoveryTransport hostTransport, clientTransport;
			NetLockstepCoordinator host, client;
			NetLockstepConfig hostConfig, clientConfig;
			uint64_t now = 0;
			uint16_t port;

			Pair(uint16_t testPort, uint16_t hostDelay, uint16_t clientDelay, bool resume = true) : port(testPort) {
				for (uint8_t peer: {uint8_t{1}, uint8_t{2}}) {
					auto& config = peer == 1 ? hostConfig : clientConfig;
					config.sessionId = 0x5253594E43000000ULL + port;
					config.startFrame = c_Start;
					config.inputDelayFrames = peer == 1 ? hostDelay : clientDelay;
					config.peerInputDelayFrames = {{1, hostDelay}, {2, clientDelay}};
					config.timeoutMs = 1000;
					config.localPeerId = peer;
					config.remotePeerId = peer == 1 ? 2 : 1;
					config.remoteTransportPeerId = 1;
					config.relayToOtherPeers = peer == 1;
					config.scenario = "ResyncRuntimeSelfTest";
					config.ownershipPolicy = "unique-id-split";
					config.resumeFromSnapshot = resume;
					config.seatPresenceEpoch[0] = 37;
					config.matchConfig.sessionId = config.sessionId;
					config.matchConfig.ownershipPolicy = NetActorOwnershipPolicy::UniqueIdModPeerCount;
				}
				hostConfig.roundId = 0x5253594E44000000ULL + port;
			}

			bool Start(std::string* error, bool discardHostStart = false) {
				if (!hostTransport.StartHost(port, error) || !clientTransport.Connect("loopback", port, error) ||
				    !host.Start(hostTransport, hostConfig, error)) return false;
				if (discardHostStart) {
					bool found = false;
					for (const auto& event: clientTransport.PollEvents()) {
						if (event.type != NetTransportEventType::PacketReceived) continue;
						const auto decoded = NetLockstepCodec::Decode(event.bytes);
						found |= decoded.ok && std::holds_alternative<NetLockstepStart>(decoded.packet.payload);
					}
					if (!Check(found, error, "lost-start fixture did not discard a real host start")) return false;
				}
				return client.Start(clientTransport, clientConfig, error);
			}

			void Step() {
				host.Tick(now);
				client.Tick(now);
				hostTransport.AdvanceTimeMs(5);
				clientTransport.AdvanceTimeMs(5);
				now += 5;
			}

			bool Until(const std::function<bool()>& done, std::string* error, uint64_t budget = 100) {
				const uint64_t end = now + budget;
				do {
					Step();
					if (done()) return true;
				} while (now <= end);
				*error = "loopback condition failed: host=" + host.BuildReportJson() + " client=" + client.BuildReportJson();
				return false;
			}

			bool Running(std::string* error, uint64_t budget = 100) {
				return Until([&] { return host.IsRunning() && client.IsRunning(); }, error, budget);
			}

			bool NothingReady(std::string* error) {
				for (int i = 0; i < 4; ++i) Step();
				NetLockstepReadyFrame ready;
				return Check(host.IsRunning() && client.IsRunning() && !host.PopReadyFrame(ready) && !client.PopReadyFrame(ready) &&
				             host.GetStats().nextFrame == c_Start && client.GetStats().nextFrame == c_Start,
				             error, "a snapshot frame committed without explicit input from every peer");
			}
		};

		Batches Recovery(uint8_t peer, uint16_t delay, CommandsByFrame& expected) {
			Batches batches(delay);
			for (uint16_t index = 0; index < delay; ++index) {
				if (delay == 1 || index != 0) batches[index].push_back(Command(peer, 100 + index));
				expected[c_Start + index] = batches[index];
			}
			return batches;
		}

		bool RefuseWrongCounts(NetLockstepCoordinator& coordinator, uint16_t delay, std::string* error) {
			const auto sent = coordinator.GetStats().framePacketsSent;
			std::string rejected;
			if (!Check(!coordinator.PrimeResyncFrames(Batches(delay + 1), &rejected) && !rejected.empty(), error, "too many recovery batches were accepted")) return false;
			if (delay != 0) {
				rejected.clear();
				if (!Check(!coordinator.PrimeResyncFrames(Batches(delay - 1), &rejected) && !rejected.empty(), error, "missing recovery batches were accepted")) return false;
				Batches wrongSender(delay);
				wrongSender.back().push_back(Command(coordinator.GetConfig().remotePeerId, 100));
				rejected.clear();
				if (!Check(!coordinator.PrimeResyncFrames(wrongSender, &rejected) && !rejected.empty(), error, "recovery accepted another peer's command")) return false;
			}
			return Check(coordinator.GetStats().framePacketsSent == sent, error, "a rejected recovery batch sent part of its frames");
		}

		bool RefuseRepeat(NetLockstepCoordinator& coordinator, const Batches& batches, std::string* error) {
			const auto sent = coordinator.GetStats().framePacketsSent;
			std::string rejected;
			return Check(!coordinator.PrimeResyncFrames(batches, &rejected) && !rejected.empty() && coordinator.GetStats().framePacketsSent == sent,
			             error, "recovery priming was accepted more than once");
		}

		bool QueueOrdinary(NetLockstepCoordinator& coordinator, uint8_t peer, uint64_t produced, CommandsByFrame& expected, std::string* error) {
			const uint64_t target = produced + coordinator.GetConfig().inputDelayFrames;
			const std::vector<NetGameCommand> commands{Command(peer, 100 + target - c_Start)};
			if (!coordinator.QueueLocalInput(produced, {Input(peer, target)}, commands, error)) return false;
			expected[target] = commands;
			return true;
		}

		bool CheckReady(const NetLockstepReadyFrame& ready, uint8_t localPeer, uint64_t target, const Pair& pair,
		                const CommandsByFrame& hostCommands, const CommandsByFrame& clientCommands, std::string* error) {
			if (!Check(ready.frame == target, error, "recovered frame changed target or committed twice")) return false;
			for (uint8_t peer: {uint8_t{1}, uint8_t{2}}) {
				const auto& expected = peer == 1 ? hostCommands : clientCommands;
				const auto& commands = peer == localPeer ? ready.localCommands : ready.remoteCommands;
				const auto& frames = peer == localPeer ? ready.localFrames : ready.remoteFrames;
				const auto delay = peer == 1 ? pair.hostConfig.inputDelayFrames : pair.clientConfig.inputDelayFrames;
				if (!Check(commands == expected.at(target), error, "command identity, order or target changed for peer " + std::to_string(peer))) return false;
				if (target < c_Start + delay) {
					if (!Check(frames.empty(), error, "a recovery frame carried newly sampled controller input")) return false;
				} else if (!Check(frames.size() == 1 && ControllerFrameCodec::Encode(frames.front()) == ControllerFrameCodec::Encode(Input(peer, target)),
				                  error, "ordinary input did not land at produced frame plus sender delay")) return false;
			}
			return Check(ready.localObservations.empty() && ready.remoteObservations.empty(), error, "recovery invented sound observations");
		}

		bool Collect(Pair& pair, uint64_t last, const CommandsByFrame& hostCommands, const CommandsByFrame& clientCommands, std::string* error) {
			uint64_t hostNext = c_Start, clientNext = c_Start;
			bool valid = true;
			if (!pair.Until([&] {
				for (uint8_t peer: {uint8_t{1}, uint8_t{2}}) {
					auto& coordinator = peer == 1 ? pair.host : pair.client;
					auto& next = peer == 1 ? hostNext : clientNext;
					NetLockstepReadyFrame ready;
					while (coordinator.PopReadyFrame(ready)) {
						if (!Check(next <= last, error, "recovery emitted an extra ready frame") ||
						    !CheckReady(ready, peer, next, pair, hostCommands, clientCommands, error)) { valid = false; return true; }
						++next;
					}
				}
				return hostNext == last + 1 && clientNext == last + 1;
			}, error)) return false;
			if (!valid) return false;
			for (int i = 0; i < 4; ++i) pair.Step();
			NetLockstepReadyFrame extra;
			return Check(pair.host.IsRunning() && pair.client.IsRunning() && !pair.host.PopReadyFrame(extra) && !pair.client.PopReadyFrame(extra) &&
			             pair.host.GetStats().nextFrame == last + 1 && pair.client.GetStats().nextFrame == last + 1,
			             error, "recovery did not end at the exact common input boundary");
		}

		bool TestAsymmetricPriming(uint16_t hostDelay, uint16_t clientDelay, uint16_t port, std::string* error) {
			Pair pair(port, hostDelay, clientDelay);
			if (!pair.Start(error) || !pair.Running(error) || !pair.NothingReady(error)) return false;
			if (!Check(pair.host.GetStats().effectiveStartFrame == c_Start && pair.client.GetStats().effectiveStartFrame == c_Start &&
			           pair.client.GetRoundId() == pair.hostConfig.roundId, error, "snapshot start or round was not negotiated")) return false;
			for (auto* coordinator: {&pair.host, &pair.client}) {
				std::string rejected;
				if (!Check(!coordinator->QueueLocalInput(c_Start, {}, {}, &rejected) && !rejected.empty() && coordinator->GetStats().framePacketsSent == 0,
				           error, "ordinary input bypassed required recovery priming")) return false;
			}
			if (!RefuseWrongCounts(pair.host, hostDelay, error) || !RefuseWrongCounts(pair.client, clientDelay, error)) return false;
			CommandsByFrame hostCommands, clientCommands;
			const auto hostBatches = Recovery(1, hostDelay, hostCommands);
			const auto clientBatches = Recovery(2, clientDelay, clientCommands);
			if (!pair.host.PrimeResyncFrames(hostBatches, error) || !QueueOrdinary(pair.host, 1, c_Start, hostCommands, error) || !pair.NothingReady(error)) return false;
			if (!pair.client.PrimeResyncFrames(clientBatches, error) || !RefuseRepeat(pair.host, hostBatches, error) || !RefuseRepeat(pair.client, clientBatches, error)) return false;
			const uint64_t last = c_Start + std::max(hostDelay, clientDelay) + 2;
			for (uint64_t produced = c_Start + 1; produced + hostDelay <= last; ++produced) {
				if (!QueueOrdinary(pair.host, 1, produced, hostCommands, error)) return false;
			}
			for (uint64_t produced = c_Start; produced + clientDelay <= last; ++produced) {
				if (!QueueOrdinary(pair.client, 2, produced, clientCommands, error)) return false;
			}
			return Collect(pair, last, hostCommands, clientCommands, error);
		}

		bool TestStartMode(std::string* error) {
			{
				Pair pair(44184, 2, 3);
				pair.clientConfig.resumeFromSnapshot = false;
				if (!pair.Start(error) || !pair.Until([&] { return pair.host.IsFailed() || pair.client.IsFailed(); }, error)) return false;
				if (!Check(pair.host.GetStats().timeoutReason.find("start mismatch") != std::string::npos || pair.client.GetStats().timeoutReason.find("start mismatch") != std::string::npos,
				           error, "different recovery modes did not fail the start handshake")) return false;
			}
			Pair ordinary(44185, 2, 3, false);
			if (!ordinary.Start(error) || !ordinary.Running(error)) return false;
			std::string rejected;
			if (!Check(!ordinary.host.PrimeResyncFrames(Batches(2), &rejected) && !rejected.empty(), error, "ordinary round accepted recovery priming")) return false;
			if (!ordinary.host.QueueLocalInput(c_Start, {}, {}, error) || !ordinary.client.QueueLocalInput(c_Start, {}, {}, error)) return false;
			return Check(ordinary.host.GetStats().effectiveStartFrame == c_Start + 2 && ordinary.client.GetStats().effectiveStartFrame == c_Start + 2,
			             error, "ordinary round lost its existing input-delay warm-up");
		}

		bool TestLostStart(std::string* error) {
			Pair pair(44186, 2, 3);
			if (!pair.Start(error, true) || !pair.Running(error, 600)) return false;
			return Check(pair.host.GetStats().startRetransmits > 0 && pair.client.GetRoundId() == pair.hostConfig.roundId &&
			             pair.host.GetStats().effectiveStartFrame == c_Start && pair.client.GetStats().effectiveStartFrame == c_Start,
			             error, "retransmitted start lost its recovery mode or round");
		}

		bool TestPrimedRoundReadoption(std::string* error) {
			Pair pair(44187, 1, 3);
			if (!pair.Start(error) || !pair.Running(error)) return false;
			CommandsByFrame hostCommands, clientCommands;
			const auto hostBatches = Recovery(1, 1, hostCommands);
			const auto clientBatches = Recovery(2, 3, clientCommands);
			if (!pair.client.PrimeResyncFrames(clientBatches, error) || !pair.NothingReady(error)) return false;
			const auto sent = pair.client.GetStats().framePacketsSent;
			++pair.hostConfig.roundId;
			if (!pair.host.Start(pair.hostTransport, pair.hostConfig, error) ||
			    !pair.Until([&] { return pair.host.IsRunning() && pair.client.IsRunning() && pair.client.GetRoundId() == pair.hostConfig.roundId; }, error)) return false;
			if (!Check(pair.client.GetStats().roundReadoptions == 1 && pair.client.GetStats().framePacketsSent >= sent + clientBatches.size(),
			           error, "following a new authority round did not resend the primed frames")) return false;
			if (!RefuseRepeat(pair.client, clientBatches, error) || !pair.host.PrimeResyncFrames(hostBatches, error)) return false;
			const uint64_t last = c_Start + 4;
			for (uint64_t produced = c_Start; produced + 1 <= last; ++produced) {
				if (!QueueOrdinary(pair.host, 1, produced, hostCommands, error)) return false;
			}
			for (uint64_t produced = c_Start; produced + 3 <= last; ++produced) {
				if (!QueueOrdinary(pair.client, 2, produced, clientCommands, error)) return false;
			}
			return Collect(pair, last, hostCommands, clientCommands, error);
		}

		struct ResetScenario {
			~ResetScenario() {
				NetLockstepCoordinator empty;
				ScenarioRunner::SetLockstepCoordinator(&empty);
				(void)ScenarioRunner::DrainLocalGameCommands();
				ScenarioRunner::SetLockstepCoordinator(nullptr);
			}
		};

		NetResyncState SnapshotState(const Pair& pair) {
			NetResyncState state;
			state.sessionId = pair.hostConfig.sessionId;
			state.sourceRound = pair.hostConfig.roundId - 1;
			state.savedTick = c_Start - 1;
			return state;
		}

		NetGameCommand Binding(uint8_t peer, int64_t selected, const std::map<uint8_t, uint64_t>& acks = {}) {
			NetGamePlayerBindings binding;
			binding.players[0].active = binding.players[0].human = true;
			binding.players[0].team = 0;
			binding.players[0].controlledUID = selected;
			binding.appliedCommands = acks;
			return {peer, binding};
		}

		NetLockstepFrame FullInput(uint8_t peer, uint64_t target, uint64_t round, size_t observations = 3) {
			NetLockstepFrame input;
			input.senderPeerId = peer;
			input.targetFrame = target;
			input.roundId = round;
			for (int index = 0; index < 2; ++index) {
				auto frame = Input(peer, target);
				frame.actorUniqueID += index * 100;
				frame.stateMask = (uint64_t{1} << WEAPON_FIRE) | (uint64_t{1} << AIM_SHARP) | (uint64_t{1} << PRESS_PRIMARY);
				frame.analogAimX = static_cast<int16_t>(12000 + target + index);
				frame.analogAimY = static_cast<int16_t>(-9000 - target - index);
				frame.mouseDeltaX = static_cast<int16_t>(-3 - index);
				frame.mouseDeltaY = static_cast<int16_t>(7 + index);
				frame.inputMode = Controller::CIM_PLAYER;
				frame.playerRaw = static_cast<int8_t>(peer - 1);
				frame.deviceClass = static_cast<uint8_t>(Controller::WireDeviceClass::MouseKeyboard);
				frame.aimAngle = static_cast<float>(target + index) * 0.03125F;
				frame.viewPointX = static_cast<float>(target * peer);
				frame.viewPointY = -static_cast<float>(target + index);
				frame.equippedFGUniqueID = 5000 + peer * 10 + index;
				input.frames.push_back(frame);
			}
			input.commands = {Command(peer, target * 3), Command(peer, target * 3 + 1), Binding(peer, 4000 + static_cast<int64_t>(target))};
			for (size_t index = 0; index < observations; ++index) {
				input.observations.push_back({peer, 100000 + peer * 10000 + target * 5000 + index, target - 1,
				                             17 + index, index * 3, observations - index, index == 0 ? -0.0F : static_cast<float>(index % 127) / 128.0F});
			}
			return input;
		}

		bool SameInput(const NetLockstepFrame& actual, const NetLockstepFrame& expected) {
			std::vector<uint8_t> actualBytes, expectedBytes;
			return NetLockstepCodec::EncodeRecoveryInput(actual, actualBytes) && NetLockstepCodec::EncodeRecoveryInput(expected, expectedBytes) && actualBytes == expectedBytes;
		}

		bool SameInputs(std::vector<NetLockstepFrame> actual, std::vector<NetLockstepFrame> expected) {
			const auto order = [](const auto& a, const auto& b) { return std::tie(a.senderPeerId, a.targetFrame) < std::tie(b.senderPeerId, b.targetFrame); };
			std::sort(actual.begin(), actual.end(), order);
			std::sort(expected.begin(), expected.end(), order);
			return actual.size() == expected.size() && std::equal(actual.begin(), actual.end(), expected.begin(), SameInput);
		}

		bool QueueFull(NetLockstepCoordinator& coordinator, const NetLockstepFrame& input, std::string* error) {
			return coordinator.QueueLocalInput(input.targetFrame - coordinator.GetConfig().inputDelayFrames, input.frames, input.commands, error, input.observations);
		}

		NetLockstepFrame ReadyInput(const NetLockstepReadyFrame& ready, uint8_t peer, uint8_t local, uint64_t round) {
			const bool own = peer == local;
			return {peer, ready.frame, own ? ready.localFrames : ready.remoteFrames, own ? ready.localCommands : ready.remoteCommands,
			        round, own ? ready.localObservations : ready.remoteObservations};
		}

		bool CollectFull(Pair& pair, uint64_t last, const InputsByFrame& host, const InputsByFrame& client, std::string* error) {
			std::array<uint64_t, 2> next{c_Start, c_Start};
			bool valid = true;
			if (!pair.Until([&] {
				for (uint8_t local: {uint8_t{1}, uint8_t{2}}) {
					auto& coordinator = local == 1 ? pair.host : pair.client;
					NetLockstepReadyFrame ready;
					while (coordinator.PopReadyFrame(ready)) {
						const auto target = next[local - 1]++;
						if (!Check(target <= last && ready.frame == target, error, "full recovery changed its target or committed an input twice")) { valid = false; return true; }
						for (uint8_t peer: {uint8_t{1}, uint8_t{2}}) {
							const auto& expected = (peer == 1 ? host : client).at(target);
							if (!Check(SameInput(ReadyInput(ready, peer, local, coordinator.GetRoundId()), expected), error,
							           "full recovery changed controller bytes, fire/aim intent, command order or observations at " + std::to_string(target))) { valid = false; return true; }
						}
					}
				}
				return next[0] == last + 1 && next[1] == last + 1;
			}, error) || !valid) return false;
			for (int index = 0; index < 4; ++index) pair.Step();
			NetLockstepReadyFrame extra;
			return Check(!pair.host.PopReadyFrame(extra) && !pair.client.PopReadyFrame(extra) && pair.host.IsRunning() && pair.client.IsRunning() &&
			             pair.host.GetStats().nextFrame == last + 1 && pair.client.GetStats().nextFrame == last + 1 &&
			             pair.host.GetStats().observationsCarried == 0 && pair.client.GetStats().observationsCarried == 0 &&
			             pair.host.GetStats().observationsDropped == 0 && pair.client.GetStats().observationsDropped == 0,
			             error, "full recovery shifted sound observations into another input or crossed its last target");
		}

		bool TestFullAsymmetric(uint16_t hostDelay, uint16_t clientDelay, uint16_t port, std::string* error) {
			Pair pair(port, hostDelay, clientDelay);
			if (!pair.Start(error) || !pair.Running(error)) return false;
			InputsByFrame host, client;
			const uint64_t last = c_Start + std::max(hostDelay, clientDelay) + 1;
			for (uint8_t peer: {uint8_t{1}, uint8_t{2}}) {
				auto& coordinator = peer == 1 ? pair.host : pair.client;
				auto& expected = peer == 1 ? host : client;
				const auto delay = coordinator.GetConfig().inputDelayFrames;
				std::vector<NetLockstepFrame> priming;
				for (uint64_t target = c_Start; target <= last; ++target) expected[target] = FullInput(peer, target, coordinator.GetRoundId());
				for (uint16_t index = 0; index < delay; ++index) priming.push_back(expected.at(c_Start + index));
				if (!coordinator.PrimeResyncInputs(priming, error) || !coordinator.QueueRecoveredInput(expected.at(c_Start + delay), error)) return false;
				for (uint64_t target = c_Start + delay + 1; target <= last; ++target) if (!QueueFull(coordinator, expected.at(target), error)) return false;
			}
			return CollectFull(pair, last, host, client, error);
		}

		bool TestRecoveryChunkRetry(std::string* error) {
			Pair pair(44200, 3, 1);
			if (!pair.Start(error) || !pair.Running(error)) return false;
			InputsByFrame host, client;
			for (uint64_t target = c_Start; target <= c_Start + 4; ++target) {
				host[target] = FullInput(1, target, pair.host.GetRoundId(), target == c_Start + 1 ? 4096 : 3);
				client[target] = FullInput(2, target, pair.client.GetRoundId());
			}
			const std::vector<NetLockstepFrame> batch{host.at(c_Start), host.at(c_Start + 1), host.at(c_Start + 2)};
			std::vector<uint8_t> encoded;
			NetLockstepError codecError;
			NetLockstepFrame decoded;
			if (!Check(NetLockstepCodec::EncodeRecoveryInput(batch[1], encoded, &codecError) && encoded.size() > 65536 &&
			           NetLockstepCodec::DecodeRecoveryInput(encoded, decoded, &codecError) && SameInput(decoded, batch[1]),
			           error, "4096-key recovery fixture did not require multiple chunks or preserve complete bytes")) return false;
			pair.hostTransport.holdLaterChunks = true;
			if (!pair.host.PrimeResyncInputs(batch, error) || !pair.client.PrimeResyncInputs({client.at(c_Start)}, error)) return false;
			for (uint64_t target = c_Start + 1; target <= c_Start + 4; ++target) if (!QueueFull(pair.client, client.at(target), error)) return false;
			for (int index = 0; index < 4; ++index) pair.Step();
			const auto received = pair.client.CapturePendingInputs(c_Start - 1);
			if (!Check(pair.hostTransport.refusedChunks > 0 && !pair.host.NeedsResyncPriming() &&
			           pair.host.GetStats().nextFrame <= c_Start + 1 && pair.client.GetStats().nextFrame <= c_Start + 1 &&
			           std::none_of(received.begin(), received.end(), [](const auto& input) { return input.senderPeerId == 1 && input.targetFrame == c_Start + 1; }),
			           error, "a refused later chunk was not retained or an incomplete input became ready")) return false;
			const auto attempts = pair.hostTransport.recoveryAttempts;
			auto conflict = batch;
			conflict[1].frames[0].analogAimX++;
			std::string rejected;
			if (!Check(!pair.host.PrimeResyncInputs(conflict, &rejected) && !rejected.empty() && pair.hostTransport.recoveryAttempts == attempts,
			           error, "conflicting priming retry sent data or replaced the retained input")) return false;
			if (!pair.host.PrimeResyncInputs(batch, error) || !QueueFull(pair.host, host.at(c_Start + 3), error) ||
			    !pair.host.QueueRecoveredInput(host.at(c_Start + 4), error)) return false;
			pair.hostTransport.holdLaterChunks = false;
			if (!CollectFull(pair, c_Start + 4, host, client, error)) return false;
			return Check(pair.hostTransport.resumedExactChunk, error, "recovery did not retry the exact refused chunk before later queued inputs");
		}

		bool SendPacket(LoopbackTransport& transport, const NetLockstepPacket& packet, std::string* error) {
			std::vector<uint8_t> bytes;
			NetLockstepError encoding;
			if (!NetLockstepCodec::Encode(packet, bytes, &encoding)) {
				*error = "ACK fixture packet did not encode: " + encoding.message;
				return false;
			}
			return transport.Send(1, NetTransportLane::ControlReliable, bytes, error);
		}

		bool CheckAckHistory(uint8_t peer, uint64_t savedTick, uint64_t applied, std::string* error) {
			NetResyncState captured;
			if (!ScenarioRunner::CaptureNetResyncState(savedTick, captured, error)) return false;
			std::vector<NetLockstepFrame> accepted;
			std::vector<uint64_t> sequences, expected;
			for (uint64_t sequence = 8; sequence <= 10; ++sequence) {
				auto input = FullInput(peer, c_Start + 10 + sequence - 8, ScenarioRunner::GetLockstepRoundId());
				input.commands = {Command(peer, sequence)};
				accepted.push_back(std::move(input));
				if (sequence > applied) expected.push_back(sequence);
			}
			for (const auto& pending: captured.pendingCommands) {
				if (!Check(pending.command.senderPeerId == peer && pending.frame == c_Start + 10 + pending.command.sequence - 8 &&
				           pending.command == Command(peer, pending.command.sequence), error, "ACK changed an accepted command's owner, target or payload")) return false;
				sequences.push_back(pending.command.sequence);
			}
			return Check(captured.appliedCommands.at(peer) == applied && sequences == expected && SameInputs(captured.pendingInputs, accepted),
			             error, "ACK or local application changed accepted input bytes or ignored the local applied watermark");
		}

		bool CheckOutbox(uint8_t peer, uint64_t savedTick, const std::vector<uint64_t>& expected, std::string* error) {
			const auto outstanding = ScenarioRunner::CaptureUnacknowledgedLocalCommands();
			std::vector<uint64_t> sequences;
			for (const auto& pending: outstanding) {
				if (!Check(pending.command.senderPeerId == peer && pending.frame == c_Start + 10 + pending.command.sequence - 8 &&
				           pending.command == Command(peer, pending.command.sequence), error, "ACK changed an outstanding command's owner, target or payload")) return false;
				sequences.push_back(pending.command.sequence);
			}
			return Check(sequences == expected, error, "ACK retained or discarded the wrong outstanding command identities") &&
			       CheckAckHistory(peer, savedTick, 7, error);
		}

		bool CheckAckApplication(uint8_t peer, uint64_t savedTick, std::string* error) {
			const auto outstanding = ScenarioRunner::CaptureUnacknowledgedLocalCommands();
			for (uint64_t sequence = 8; sequence <= 10; ++sequence) {
				if (!Check(ScenarioRunner::ConsumeLockstepGameCommand(Command(peer, sequence)) &&
				           !ScenarioRunner::ConsumeLockstepGameCommand(Command(peer, sequence)), error, "local command application did not advance once") ||
				    !CheckAckHistory(peer, savedTick, sequence, error)) return false;
			}
			return Check(ScenarioRunner::CaptureUnacknowledgedLocalCommands() == outstanding, error, "local application changed retry ownership without an ACK");
		}

		bool SeedOutbox(const Pair& pair, uint8_t peer, std::string* error) {
			auto state = SnapshotState(pair);
			state.appliedCommands[peer] = 7;
			for (uint64_t sequence = 8; sequence <= 10; ++sequence) {
				const uint64_t target = c_Start + 10 + sequence - 8;
				state.pendingCommands.push_back({target, Command(peer, sequence)});
				auto input = FullInput(peer, target, state.sourceRound);
				input.commands = {Command(peer, sequence)};
				state.pendingInputs.push_back(std::move(input));
			}
			for (uint64_t frame = c_Start; frame <= c_Start + 3; ++frame) state.pendingPlayerBindings.push_back({frame, Binding(peer, 4242)});
			return ScenarioRunner::RestoreNetResyncState(state, error) && CheckOutbox(peer, c_Start - 1, {8, 9, 10}, error);
		}

		bool ObserveRemoteBinding(Pair& pair, uint8_t local, uint64_t target, const std::map<uint8_t, uint64_t>& acks,
		                          const std::vector<uint64_t>& before, const std::vector<uint64_t>& after, std::string* error) {
			auto& receiver = local == 1 ? pair.host : pair.client;
			auto& sender = local == 1 ? pair.client : pair.host;
			const uint8_t remote = local == 1 ? 2 : 1;
			const auto binding = Binding(remote, 4344, acks);
			if (!receiver.QueueLocalInput(target, {}, {}, error) || !sender.QueueLocalInput(target, {}, {binding}, error) ||
			    !pair.Until([&] { return receiver.GetStats().nextFrame == target + 1 && sender.GetStats().nextFrame == target + 1; }, error)) return false;
			if (!CheckOutbox(local, target, before, error)) return false;
			NetLockstepReadyFrame ready, other;
			if (!Check(receiver.PopReadyFrame(ready) && sender.PopReadyFrame(other) && ready.frame == target && other.frame == target &&
			           ready.localCommands.empty() && ready.remoteCommands == std::vector<NetGameCommand>{binding},
			           error, "binding acknowledgement did not arrive as the authenticated remote's committed command")) return false;
			ScenarioRunner::ObserveLockstepPlayerBindings(ready.remoteCommands.front().senderPeerId, ready.frame,
			                                                std::get<NetGamePlayerBindings>(ready.remoteCommands.front().payload));
			return CheckOutbox(local, target, after, error);
		}

		bool TestBindingAckAuthority(std::string* error) {
			for (uint8_t local: {uint8_t{1}, uint8_t{2}}) {
				Pair pair(static_cast<uint16_t>(44190 + local), 0, 0);
				if (!pair.Start(error) || !pair.Running(error)) return false;
				auto& receiver = local == 1 ? pair.host : pair.client;
				ResetScenario reset;
				ScenarioRunner::SetLockstepCoordinator(&receiver);
				if (!SeedOutbox(pair, local, error) || !pair.host.PrimeResyncFrames({}, error) || !pair.client.PrimeResyncFrames({}, error)) return false;
				if (local == 1) {
					if (!ObserveRemoteBinding(pair, local, c_Start, {{local, 10}}, {8, 9, 10}, {8, 9, 10}, error)) return false;
				} else {
					if (!ObserveRemoteBinding(pair, local, c_Start, {{local, 8}}, {8, 9, 10}, {9, 10}, error) ||
					    !ObserveRemoteBinding(pair, local, c_Start + 1, {{local, 7}}, {9, 10}, {9, 10}, error) ||
					    !ObserveRemoteBinding(pair, local, c_Start + 2, {{local, 10}}, {9, 10}, {}, error)) return false;
				}
				if (!CheckAckApplication(local, local == 1 ? c_Start : c_Start + 2, error)) return false;
			}
			return true;
		}

		bool TestChecksumAckAuthority(std::string* error) {
			for (uint8_t local: {uint8_t{1}, uint8_t{2}}) {
				Pair pair(static_cast<uint16_t>(44192 + local), 0, 0);
				if (!pair.Start(error) || !pair.Running(error)) return false;
				auto& receiver = local == 1 ? pair.host : pair.client;
				ResetScenario reset;
				ScenarioRunner::SetLockstepCoordinator(&receiver);
				if (!SeedOutbox(pair, local, error)) return false;
				NetLockstepChecksum packet;
				packet.senderPeerId = 1;
				packet.frame = c_Start;
				packet.roundId = pair.host.GetRoundId();
				packet.appliedCommands = {{local, 10}};
				if (local == 1) {
					LoopbackTransport stranger;
					if (!stranger.Connect("loopback", pair.port, error)) return false;
					packet.senderPeerId = 2;
					--packet.roundId;
					if (!SendPacket(stranger, {packet}, error)) return false;
					for (int i = 0; i < 4; ++i) pair.Step();
					if (!Check(receiver.IsRunning() && receiver.GetStats().staleRoundPackets == 0 && receiver.GetAuthoritativeCommandAcks().empty(),
					           error, "an unbound transport was accepted as an existing checksum sender")) return false;
					if (!SendPacket(pair.clientTransport, {packet}, error)) return false;
					for (int i = 0; i < 4; ++i) pair.Step();
					if (!Check(receiver.IsRunning() && receiver.GetStats().staleRoundPackets == 1,
					           error, "authenticated checksum control did not reach its sender's round check")) return false;
					++packet.roundId;
					if (!SendPacket(pair.clientTransport, {packet}, error)) return false;
					for (int i = 0; i < 4; ++i) pair.Step();
					if (!Check(receiver.IsRunning() && receiver.GetAuthoritativeCommandAcks().empty(), error, "a non-host checksum granted authoritative acknowledgements")) return false;
					if (!ScenarioRunner::QueueLockstepLocalControllerFrames(c_Start, {}, error) || !CheckOutbox(local, c_Start, {8, 9, 10}, error)) return false;
				} else {
					--packet.roundId;
					if (!SendPacket(pair.hostTransport, {packet}, error)) return false;
					for (int i = 0; i < 4; ++i) pair.Step();
					if (!Check(receiver.IsRunning() && receiver.GetStats().staleRoundPackets == 1 && receiver.GetAuthoritativeCommandAcks().empty(),
					           error, "a previous round's checksum acknowledged current commands")) return false;
					if (!ScenarioRunner::QueueLockstepLocalControllerFrames(c_Start, {}, error) || !CheckOutbox(local, c_Start, {8, 9, 10}, error)) return false;
					const std::array<uint64_t, 3> acknowledgements{8, 7, 10};
					for (size_t index = 0; index < acknowledgements.size(); ++index) {
						const uint64_t frame = c_Start + index + 1;
						const uint64_t expectedAck = index == 2 ? 10 : 8;
						if (!pair.host.SubmitLocalChecksum(frame, {}, error, {{local, acknowledgements[index]}})) return false;
						for (int i = 0; i < 4; ++i) pair.Step();
						const auto& acks = receiver.GetAuthoritativeCommandAcks();
						if (!Check(receiver.IsRunning() && acks.size() == 1 && acks.at(local) == expectedAck, error, "host checksum ACK regressed or was not accepted")) return false;
						if (!ScenarioRunner::QueueLockstepLocalControllerFrames(frame, {}, error) ||
						    !CheckOutbox(local, frame, index == 2 ? std::vector<uint64_t>{} : std::vector<uint64_t>{9, 10}, error)) return false;
					}
				}
				if (!CheckAckApplication(local, local == 1 ? c_Start : c_Start + 3, error)) return false;
			}
			return true;
		}

		bool SamePending(std::vector<NetResyncPendingCommand> actual, std::vector<NetResyncPendingCommand> expected) {
			const auto order = [](const auto& a, const auto& b) {
				return std::tie(a.command.senderPeerId, a.frame, a.command.sequence) < std::tie(b.command.senderPeerId, b.frame, b.command.sequence);
			};
			std::sort(actual.begin(), actual.end(), order);
			std::sort(expected.begin(), expected.end(), order);
			return actual == expected;
		}

		bool TestSnapshotCapture(std::string* error) {
			Pair pair(44195, 0, 0);
			if (!pair.Start(error) || !pair.Running(error)) return false;
			ResetScenario reset;
			ScenarioRunner::SetLockstepCoordinator(&pair.host);
			auto seed = SnapshotState(pair);
			seed.controlOwners = {{4242, 2}};
			seed.droppedControlOwners = {{4344, 2}};
			seed.appliedCommands = {{1, 7}, {2, 17}};
			seed.playerBindings = {{2, {c_Start - 1, std::get<NetGamePlayerBindings>(Binding(2, 4400).payload)}}};
			seed.pendingCommands = {{c_Start + 1, Command(1, 9)}};
			if (!ScenarioRunner::RestoreNetResyncState(seed, error) || !pair.host.PrimeResyncFrames({}, error) || !pair.client.PrimeResyncFrames({}, error)) return false;
			const auto hostFirst = Binding(1, 4242), clientFirst = Binding(2, 4344);
			const auto hostNext = Binding(1, 4244), clientNext = Binding(2, 4346);
			auto hostInput = FullInput(1, c_Start, pair.host.GetRoundId()), clientInput = FullInput(2, c_Start, pair.client.GetRoundId());
			auto hostFuture = FullInput(1, c_Start + 1, pair.host.GetRoundId()), clientFuture = FullInput(2, c_Start + 2, pair.client.GetRoundId());
			hostInput.commands = {Command(1, 8), hostFirst}; clientInput.commands = {Command(2, 18), clientFirst};
			hostFuture.commands = {Command(1, 9), hostNext}; clientFuture.commands = {Command(2, 19), clientNext};
			if (!QueueFull(pair.host, hostInput, error) || !QueueFull(pair.client, clientInput, error) ||
			    !pair.Until([&] { return pair.host.GetStats().nextFrame == c_Start + 1 && pair.client.GetStats().nextFrame == c_Start + 1; }, error)) return false;
			if (!QueueFull(pair.host, hostFuture, error) || !QueueFull(pair.client, clientFuture, error)) return false;
			for (int i = 0; i < 4; ++i) pair.Step();
			if (!Check(pair.host.GetStats().framesAccepted == 1 && pair.client.GetStats().framesAccepted == 1,
			           error, "capture fixture failed to retain one ready frame and later incomplete frames")) return false;
			const std::vector<NetResyncPendingCommand> commands{{c_Start, Command(1, 8)}, {c_Start + 1, Command(1, 9)}, {c_Start, Command(2, 18)}, {c_Start + 2, Command(2, 19)}};
			const std::vector<NetResyncPendingCommand> bindings{{c_Start, hostFirst}, {c_Start + 1, hostNext}, {c_Start, clientFirst}, {c_Start + 2, clientNext}};
			const std::vector<NetLockstepFrame> inputs{hostInput, hostFuture, clientInput, clientFuture};
			if (!Check(SameInputs(pair.host.CapturePendingInputs(c_Start - 1), inputs) && SameInputs(pair.client.CapturePendingInputs(c_Start - 1), inputs) &&
			           SameInputs(pair.host.CaptureLocalInputHistory(), {hostInput, hostFuture}) && SameInputs(pair.client.CaptureLocalInputHistory(), {clientInput, clientFuture}),
			           error, "coordinator capture lost ready input, buffered input or local sender history")) return false;
			NetResyncState captured;
			if (!ScenarioRunner::CaptureNetResyncState(c_Start - 1, captured, error)) return false;
			if (!Check(captured.sessionId == pair.hostConfig.sessionId && captured.sourceRound == pair.host.GetRoundId() && captured.savedTick == c_Start - 1 &&
			           captured.controlOwners == seed.controlOwners && captured.droppedControlOwners == seed.droppedControlOwners &&
			           captured.appliedCommands == seed.appliedCommands && captured.playerBindings == seed.playerBindings &&
			           SamePending(captured.pendingCommands, commands) && SamePending(captured.pendingPlayerBindings, bindings) && SameInputs(captured.pendingInputs, inputs),
			           error, "snapshot export lost ownership, applied state, ready input or pending input")) return false;
			if (!ScenarioRunner::CaptureNetResyncState(c_Start, captured, error)) return false;
			if (!Check(SamePending(captured.pendingCommands, {{c_Start + 1, Command(1, 9)}, {c_Start + 2, Command(2, 19)}}) &&
			           SamePending(captured.pendingPlayerBindings, {{c_Start + 1, hostNext}, {c_Start + 2, clientNext}}) && SameInputs(captured.pendingInputs, {hostFuture, clientFuture}),
			           error, "snapshot export retained commands or bindings at the saved tick")) return false;
			if (!pair.host.QueueLocalInput(c_Start + 3, {}, {Command(1, 9)}, error)) return false;
			const auto before = captured;
			std::string rejected;
			return Check(!ScenarioRunner::CaptureNetResyncState(c_Start, captured, &rejected) && !rejected.empty() && captured == before,
			             error, "conflicting pending command targets were accepted or changed the capture destination");
		}

		bool TestThreePeerCapture(std::string* error) {
			std::array<LoopbackTransport, 3> transports;
			std::array<NetLockstepCoordinator, 3> coordinators;
			if (!transports[0].StartHost(44201, error) || !transports[1].Connect("loopback", 44201, error) || !transports[2].Connect("loopback", 44201, error)) return false;
			for (uint8_t peer = 1; peer <= 3; ++peer) {
				NetLockstepConfig config;
				config.sessionId = 0x5253594E43044201ULL;
				config.roundId = peer == 1 ? 424201 : 0;
				config.startFrame = c_Start;
				config.localPeerId = peer;
				config.peerCount = 3;
				config.timeoutMs = 1000;
				config.resumeFromSnapshot = true;
				config.remoteTransportPeerIds = peer == 1 ? std::map<uint8_t, NetPeerId>{{2, 1}, {3, 2}} : std::map<uint8_t, NetPeerId>{{1, 1}};
				config.relayToOtherPeers = peer == 1;
				if (!coordinators[peer - 1].Start(transports[peer - 1], config, error)) return false;
			}
			uint64_t now = 0;
			const auto drive = [&](const auto& done) {
				const uint64_t end = now + 150;
				do {
					for (auto& coordinator: coordinators) coordinator.Tick(now);
					for (auto& transport: transports) transport.AdvanceTimeMs(5);
					now += 5;
					if (done()) return true;
				} while (now <= end);
				return false;
			};
			if (!Check(drive([&] { return std::all_of(coordinators.begin(), coordinators.end(), [](const auto& item) { return item.IsRunning(); }); }), error,
			           "three-peer recovery capture did not start")) return false;
			std::vector<NetLockstepFrame> expected;
			for (uint8_t peer = 1; peer <= 3; ++peer) {
				auto input = FullInput(peer, c_Start, coordinators[peer - 1].GetRoundId());
				input.commands.clear(); input.observations.clear();
				if (!coordinators[peer - 1].PrimeResyncInputs({}, error) || !QueueFull(coordinators[peer - 1], input, error)) return false;
				expected.push_back(std::move(input));
			}
			if (!Check(drive([&] { return std::all_of(coordinators.begin(), coordinators.end(), [](const auto& item) { return item.GetStats().nextFrame == c_Start + 1; }); }),
			           error, "three-peer recovery capture did not retain a ready frame")) return false;
			for (const auto& coordinator: coordinators) {
				if (!Check(SameInputs(coordinator.CapturePendingInputs(c_Start - 1), expected), error,
				           "ready controller-only inputs lost their original sender when remote inputs were merged")) return false;
			}
			return true;
		}

		bool TestLocalHistoryBound(std::string* error) {
			Pair pair(44202, 0, 0);
			if (!pair.Start(error) || !pair.Running(error) || !pair.host.PrimeResyncInputs({}, error) || !pair.client.PrimeResyncInputs({}, error)) return false;
			constexpr auto kept = NetLockstepCodec::c_MaxFutureFrameSkew + 1;
			const uint64_t last = c_Start + kept + 3;
			std::vector<NetLockstepFrame> expected;
			for (uint64_t target = c_Start; target <= last; ++target) {
				const auto input = FullInput(1, target, pair.host.GetRoundId());
				if (!QueueFull(pair.host, input, error) || !pair.client.QueueLocalInput(target, {}, {}, error) ||
				    !pair.Until([&] { return pair.host.GetStats().nextFrame == target + 1 && pair.client.GetStats().nextFrame == target + 1; }, error)) return false;
				NetLockstepReadyFrame ready;
				if (!Check(pair.host.PopReadyFrame(ready) && pair.client.PopReadyFrame(ready), error, "history bound fixture did not consume its ready input")) return false;
				if (target > last - kept) expected.push_back(input);
			}
			return Check(SameInputs(pair.host.CaptureLocalInputHistory(), expected), error, "local input history lost consumed input or exceeded the inclusive pending window");
		}

		bool RestartRound(Pair& pair, std::string* error) {
			++pair.hostConfig.roundId;
			return pair.host.Start(pair.hostTransport, pair.hostConfig, error) && pair.client.Start(pair.clientTransport, pair.clientConfig, error) && pair.Running(error);
		}

		NetResyncState InputState(const Pair& pair, std::vector<NetLockstepFrame> inputs) {
			auto state = SnapshotState(pair);
			for (auto& input: inputs) {
				input.roundId = state.sourceRound;
				for (const auto& command: input.commands) {
					if (std::holds_alternative<NetGamePlayerBindings>(command.payload)) state.pendingPlayerBindings.push_back({input.targetFrame, command});
					else if (command.sequence != 0) state.pendingCommands.push_back({input.targetFrame, command});
				}
			}
			state.pendingInputs = std::move(inputs);
			return state;
		}

		bool RefuseRestoreUnchanged(Pair& pair, NetLockstepCoordinator& receiver, const NetResyncState& rejectedState, const std::string& name, std::string* error) {
			NetResyncState before, after;
			std::vector<uint8_t> encoded, beforeBytes, afterBytes;
			if (!NetResyncCodec::Encode(rejectedState, {0x52}, encoded, error)) { *error = name + " fixture is not individually encodable: " + *error; return false; }
			if (!ScenarioRunner::CaptureNetResyncState(c_Start - 1, before, error) || !NetResyncCodec::Encode(before, {0x52}, beforeBytes, error)) return false;
			const auto inputs = receiver.CapturePendingInputs(c_Start - 1), history = receiver.CaptureLocalInputHistory();
			const auto report = receiver.BuildReportJson();
			const auto attempts = pair.hostTransport.recoveryAttempts + pair.clientTransport.recoveryAttempts;
			const bool priming = receiver.NeedsResyncPriming();
			std::string rejected;
			if (!Check(!ScenarioRunner::RestoreNetResyncState(rejectedState, &rejected) && !rejected.empty(), error, name + " was accepted")) return false;
			if (!ScenarioRunner::CaptureNetResyncState(c_Start - 1, after, error) || !NetResyncCodec::Encode(after, {0x52}, afterBytes, error)) return false;
			return Check(beforeBytes == afterBytes && SameInputs(receiver.CapturePendingInputs(c_Start - 1), inputs) &&
			             SameInputs(receiver.CaptureLocalInputHistory(), history) && receiver.BuildReportJson() == report && receiver.NeedsResyncPriming() == priming &&
			             pair.hostTransport.recoveryAttempts + pair.clientTransport.recoveryAttempts == attempts,
			             error, name + " changed captured state, coordinator queues or transport after refusal");
		}

		bool TestAuthoritativeInputRestore(std::string* error) {
			for (uint8_t local: {uint8_t{1}, uint8_t{2}}) {
				Pair pair(static_cast<uint16_t>(44205 + local), 0, 0);
				if (!pair.Start(error) || !pair.Running(error) || !pair.host.PrimeResyncInputs({}, error) || !pair.client.PrimeResyncInputs({}, error)) return false;
				ResetScenario reset;
				ScenarioRunner::SetLockstepCoordinator(&pair.host);
				std::vector<NetLockstepFrame> expected;
				for (uint64_t target = c_Start; target <= c_Start + 1; ++target) for (uint8_t peer: {uint8_t{1}, uint8_t{2}}) {
					expected.push_back(FullInput(peer, target, pair.host.GetRoundId()));
					if (!QueueFull(peer == 1 ? pair.host : pair.client, expected.back(), error)) return false;
				}
				if (!pair.Until([&] { return pair.host.GetStats().nextFrame == c_Start + 2 && pair.client.GetStats().nextFrame == c_Start + 2; }, error)) return false;
				const auto acknowledged = Command(2, c_Start * 3);
				if (!Check(ScenarioRunner::ConsumeLockstepGameCommand(acknowledged), error, "duplicate-command fixture did not establish its watermark")) return false;
				NetResyncState captured;
				if (!ScenarioRunner::CaptureNetResyncState(c_Start - 1, captured, error) ||
				    !Check(SameInputs(captured.pendingInputs, expected) && std::none_of(captured.pendingCommands.begin(), captured.pendingCommands.end(), [&](const auto& command) {
					    return command.command.senderPeerId == acknowledged.senderPeerId && command.command.sequence == acknowledged.sequence;
				    }), error, "capture altered an acknowledged duplicate inside its historical input")) return false;
				ScenarioRunner::SetLockstepCoordinator(nullptr);
				if (!RestartRound(pair, error)) return false;
				auto& receiver = local == 1 ? pair.host : pair.client;
				auto& absent = local == 1 ? pair.client : pair.host;
				ScenarioRunner::SetLockstepCoordinator(&receiver);
				for (auto& input: expected) input.roundId = receiver.GetRoundId();
				const auto attempts = pair.hostTransport.recoveryAttempts + pair.clientTransport.recoveryAttempts;
				if (!ScenarioRunner::RestoreNetResyncState(captured, error) ||
				    !Check(SameInputs(receiver.CapturePendingInputs(c_Start - 1), expected), error, "restore did not seed both peers' exact accepted inputs")) return false;
				if (!receiver.PrimeResyncInputs({}, error)) return false;
				receiver.Tick(pair.now);
				for (uint64_t target = c_Start; target <= c_Start + 1; ++target) {
					NetLockstepReadyFrame ready;
					if (!Check(receiver.PopReadyFrame(ready) && ready.frame == target, error, "restored input waited for an absent sender to resend")) return false;
					for (const auto& input: expected) if (input.targetFrame == target &&
					    !Check(SameInput(ReadyInput(ready, input.senderPeerId, local, receiver.GetRoundId()), input), error, "installed input changed before commitment")) return false;
				}
				NetLockstepReadyFrame extra;
				if (!Check(!receiver.PopReadyFrame(extra) && receiver.GetStats().nextFrame == c_Start + 2 && absent.GetStats().nextFrame == c_Start &&
				           receiver.GetStats().framePacketsSent == 0 && absent.GetStats().framePacketsSent == 0 &&
				           pair.hostTransport.recoveryAttempts + pair.clientTransport.recoveryAttempts == attempts &&
				           !ScenarioRunner::ConsumeLockstepGameCommand(acknowledged) && ScenarioRunner::ConsumeLockstepGameCommand(Command(2, acknowledged.sequence + 1)) &&
				           !ScenarioRunner::ConsumeLockstepGameCommand(Command(2, acknowledged.sequence + 1)),
				           error, "authoritative restore resent input, crossed its target boundary or reapplied an acknowledged command")) return false;
			}
			return true;
		}

		bool TestRestoreMetadataRefusals(std::string* error) {
			Pair pair(44208, 0, 0);
			if (!pair.Start(error) || !pair.Running(error)) return false;
			ResetScenario reset;
			ScenarioRunner::SetLockstepCoordinator(&pair.host);
			ScenarioRunner::SetLockstepControlOverride(4242, 2);
			const auto unsent = Command(1, 0);
			ScenarioRunner::EnqueueLocalGameCommand(unsent);
			auto host = FullInput(1, c_Start, pair.host.GetRoundId());
			host.commands[0] = {1, NetGameAIOrder{4242, 0, NetGameAIOrder::SceneWaypoint, 0.0F, 0.0F, 0}, c_Start * 3};
			auto valid = InputState(pair, {host, FullInput(2, c_Start + 1, pair.host.GetRoundId())});
			valid.controlOwners = {{4242, 1}};
			valid.droppedControlOwners = {{4344, 2}};
			const std::vector<std::pair<std::string, std::function<void(NetResyncState&)>>> mutations{
				{"full input disagrees with command payload", [](auto& state) { std::get<NetGameAIOrder>(state.pendingCommands[0].command.payload).x = 1.0F; }},
				{"full input disagrees with command signed zero", [](auto& state) { std::get<NetGameAIOrder>(state.pendingCommands[0].command.payload).x = -0.0F; }},
				{"full input disagrees with binding selection", [](auto& state) { ++std::get<NetGamePlayerBindings>(state.pendingPlayerBindings[0].command.payload).players[0].controlledUID; }},
				{"full input disagrees with binding signed zero", [](auto& state) { std::get<NetGamePlayerBindings>(state.pendingPlayerBindings[0].command.payload).players[0].cameraX = -0.0F; }},
				{"accepted command moved to another target", [](auto& state) { for (auto& pending: state.pendingCommands) if (pending.command.senderPeerId == 1) ++pending.frame; }},
				{"command metadata is absent from full input", [](auto& state) { state.pendingCommands.push_back({c_Start, Command(1, c_Start * 3 + 2)}); }},
				{"binding metadata is absent from full input", [](auto& state) { state.pendingInputs[0].commands.pop_back(); }},
				{"sender and sequence reused by a later input", [](auto& state) { auto later = state.pendingInputs[0]; later.targetFrame += 2; state.pendingInputs.push_back(std::move(later)); }},
				{"later accepted input overtakes a lower sequence", [](auto& state) {
					auto later = state.pendingInputs[0]; ++later.targetFrame; later.commands = {Command(1, c_Start * 3 - 1)}; state.pendingInputs.push_back(std::move(later));
				}}
			};
			for (const auto& [name, mutate]: mutations) {
				auto bad = valid;
				mutate(bad);
				if (!RefuseRestoreUnchanged(pair, pair.host, bad, name, error) ||
				    !Check(ScenarioRunner::DrainLocalGameCommands() == std::vector<NetGameCommand>{unsent}, error, name + " changed unsent local intent")) return false;
				ScenarioRunner::EnqueueLocalGameCommand(unsent);
			}
			auto expected = valid.pendingInputs;
			for (auto& input: expected) input.roundId = pair.host.GetRoundId();
			return ScenarioRunner::RestoreNetResyncState(valid, error) &&
			       Check(SameInputs(pair.host.CapturePendingInputs(c_Start - 1), expected) && ScenarioRunner::DrainLocalGameCommands() == std::vector<NetGameCommand>{unsent},
			             error, "refused restores poisoned the following valid installation");
		}

		bool TestRestoreHistoryConflicts(std::string* error) {
			Pair pair(44209, 0, 0);
			if (!pair.Start(error) || !pair.Running(error) || !pair.client.PrimeResyncInputs({}, error)) return false;
			ResetScenario reset;
			ScenarioRunner::SetLockstepCoordinator(&pair.client);
			auto local = FullInput(2, c_Start, pair.client.GetRoundId());
			local.frames[0].aimAngle = 0.0F;
			local.commands[0] = {2, NetGameAIOrder{4242, 0, NetGameAIOrder::SceneWaypoint, 0.0F, 0.0F, 0}, c_Start * 3};
			if (!QueueFull(pair.client, local, error)) return false;
			ScenarioRunner::SetLockstepCoordinator(nullptr);
			if (!RestartRound(pair, error)) return false;
			ScenarioRunner::SetLockstepCoordinator(&pair.client, true);
			const std::vector<std::pair<std::string, std::function<void(NetLockstepFrame&)>>> mutations{
				{"host input conflicts with local fire intent", [](auto& input) { input.frames[0].stateMask ^= uint64_t{1} << WEAPON_FIRE; }},
				{"host input conflicts with local controller signed zero", [](auto& input) { input.frames[0].aimAngle = -0.0F; }},
				{"host input conflicts with local sound signed zero", [](auto& input) { input.observations[0].value = 0.0F; }},
				{"host input conflicts with local command signed zero", [](auto& input) { std::get<NetGameAIOrder>(input.commands[0].payload).x = -0.0F; }},
				{"host input conflicts with local binding signed zero", [](auto& input) { std::get<NetGamePlayerBindings>(input.commands.back().payload).players[0].cameraX = -0.0F; }}
			};
			for (const auto& [name, mutate]: mutations) {
				auto conflict = local;
				mutate(conflict);
				if (!RefuseRestoreUnchanged(pair, pair.client, InputState(pair, {conflict}), name, error)) return false;
			}
			const auto valid = InputState(pair, {local});
			local.roundId = pair.client.GetRoundId();
			return ScenarioRunner::RestoreNetResyncState(valid, error) &&
			       Check(SameInputs(pair.client.CapturePendingInputs(c_Start - 1), {local}), error, "matching local history did not survive preceding refused restores");
		}

		bool TestUnsentIntentTargets(std::string* error) {
			for (uint8_t kind = 0; kind < 3; ++kind) {
				Pair pair(static_cast<uint16_t>(44210 + kind), 0, 0);
				if (!pair.Start(error) || !pair.Running(error)) return false;
				ResetScenario reset;
				ScenarioRunner::SetLockstepCoordinator(&pair.host);
				const auto intent = Command(1, kind == 1 ? 2 : 1), accepted = Command(1, kind == 1 ? 1 : 2);
				ScenarioRunner::EnqueueLocalGameCommand(intent);
				std::string rejected;
				if (!Check(!ScenarioRunner::QueueLockstepLocalControllerFrames(c_Start - 1, {}, &rejected) && !rejected.empty() &&
				           pair.host.GetStats().framePacketsSent == 0 && pair.host.CaptureLocalInputHistory().empty() &&
				           ScenarioRunner::DrainLocalGameCommands() == std::vector<NetGameCommand>{intent},
				           error, "overdue intent fixture accidentally accepted an input or lost its failed command")) return false;
				ScenarioRunner::SetLockstepCoordinator(nullptr);
				if (!RestartRound(pair, error)) return false;
				ScenarioRunner::SetLockstepCoordinator(&pair.host, true);
				auto occupied = FullInput(1, c_Start, pair.host.GetRoundId());
				occupied.commands.clear();
				std::vector<NetLockstepFrame> inputs{occupied};
				if (kind == 2) {
					auto barrier = FullInput(1, c_Start + 1, pair.host.GetRoundId());
					barrier.commands = {accepted};
					inputs.push_back(std::move(barrier));
				}
				auto state = InputState(pair, inputs);
				if (kind != 2) state.pendingCommands.push_back({c_Start + 1, accepted});
				if (kind == 2) {
					if (!RefuseRestoreUnchanged(pair, pair.host, state, "unsent intent cannot overtake the next accepted input", error)) return false;
					continue;
				}
				if (!ScenarioRunner::RestoreNetResyncState(state, error)) return false;
				NetResyncState captured;
				if (!ScenarioRunner::CaptureNetResyncState(c_Start - 1, captured, error) ||
				    !Check(SameInputs(captured.pendingInputs, {occupied}) &&
				           SamePending(captured.pendingCommands, {{c_Start + 1, Command(1, 1)}, {c_Start + 1, Command(1, 2)}}),
				           error, "unsent intent moved an accepted input or crossed a lower or higher command sequence")) return false;
				if (!ScenarioRunner::QueueLockstepLocalControllerFrames(c_Start, {Input(1, c_Start + 99)}, error) ||
				    !ScenarioRunner::QueueLockstepLocalControllerFrames(c_Start + 1, {Input(1, c_Start + 1)}, error)) return false;
				NetLockstepFrame resumed{1, c_Start + 1, {Input(1, c_Start + 1)}, {Command(1, 1), Command(1, 2)}, pair.host.GetRoundId(), {}};
				if (!Check(SameInputs(pair.host.CapturePendingInputs(c_Start - 1), {occupied, resumed}), error,
				           "unsent intent did not reach the free target once in sequence order")) return false;
			}
			return true;
		}

		bool TestSecondHealBeforeResend(std::string* error) {
			Pair pair(44203, 0, 0);
			if (!pair.Start(error) || !pair.Running(error) || !pair.host.PrimeResyncInputs({}, error) || !pair.client.PrimeResyncInputs({}, error)) return false;
			ResetScenario reset;
			ScenarioRunner::SetLockstepCoordinator(&pair.host);
			auto hostInput = FullInput(1, c_Start, pair.host.GetRoundId()), remoteInput = FullInput(2, c_Start + 1, pair.client.GetRoundId());
			if (!QueueFull(pair.host, hostInput, error) || !QueueFull(pair.client, remoteInput, error)) return false;
			for (int index = 0; index < 4; ++index) pair.Step();
			NetResyncState first;
			if (!ScenarioRunner::CaptureNetResyncState(c_Start - 1, first, error) ||
			    !Check(SameInputs(first.pendingInputs, {hostInput, remoteInput}), error, "first heal did not capture both peers' accepted pending input")) return false;
			ScenarioRunner::SetLockstepCoordinator(nullptr);
			if (!RestartRound(pair, error)) return false;
			ScenarioRunner::SetLockstepCoordinator(&pair.host, true);
			if (!Check(pair.host.CapturePendingInputs(c_Start - 1).empty(), error, "second-heal fixture received input before restoration")) return false;
			const auto sent = pair.host.GetStats().framePacketsSent + pair.client.GetStats().framePacketsSent;
			const auto attempts = pair.hostTransport.recoveryAttempts + pair.clientTransport.recoveryAttempts;
			if (!ScenarioRunner::RestoreNetResyncState(first, error)) return false;
			hostInput.roundId = remoteInput.roundId = pair.host.GetRoundId();
			if (!Check(SameInputs(pair.host.CapturePendingInputs(c_Start - 1), {hostInput, remoteInput}) &&
			           pair.host.GetStats().framePacketsSent + pair.client.GetStats().framePacketsSent == sent &&
			           pair.hostTransport.recoveryAttempts + pair.clientTransport.recoveryAttempts == attempts,
			           error, "restoration did not install exact pending inputs without a wire resend")) return false;
			NetResyncState second;
			if (!ScenarioRunner::CaptureNetResyncState(c_Start - 1, second, error)) return false;
			if (!Check(second.sourceRound == pair.host.GetRoundId() && SameInputs(second.pendingInputs, {hostInput, remoteInput}) &&
			           SamePending(second.pendingCommands, first.pendingCommands) && SamePending(second.pendingPlayerBindings, first.pendingPlayerBindings),
			           error, "second heal discarded accepted remote inputs, commands or bindings before their sender resent them")) return false;
			std::vector<uint8_t> bytes, archive;
			NetResyncState decoded;
			return Check(NetResyncCodec::Encode(second, {1, 3, 5, 7}, bytes, error) &&
			             NetResyncCodec::Decode(bytes, second.sessionId, c_Start, decoded, archive, error) && decoded == second && archive == std::vector<uint8_t>({1, 3, 5, 7}),
			             error, "second-heal envelope failed to retain the complete pending input state");
		}

		bool TestRepeatedFullDelayedRestore(uint16_t hostDelay, uint16_t clientDelay, uint16_t port, std::string* error) {
			Pair pair(port, hostDelay, clientDelay);
			if (!pair.Start(error) || !pair.Running(error)) return false;
			ResetScenario reset;
			ScenarioRunner::SetLockstepCoordinator(&pair.host);
			const uint64_t last = c_Start + std::max(hostDelay, clientDelay) + 4;
			InputsByFrame host, client;
			for (uint8_t peer: {uint8_t{1}, uint8_t{2}}) {
				auto& coordinator = peer == 1 ? pair.host : pair.client;
				auto& expected = peer == 1 ? host : client;
				const uint16_t delay = coordinator.GetConfig().inputDelayFrames;
				std::vector<NetLockstepFrame> priming;
				for (uint64_t target = c_Start; target <= last; ++target) {
					expected[target] = FullInput(peer, target, coordinator.GetRoundId(), target == c_Start + 1 ? 4096 : 3);
					if (target < c_Start + delay) priming.push_back(expected.at(target));
				}
				if (!coordinator.PrimeResyncInputs(priming, error)) return false;
				for (uint64_t target = c_Start + delay; target <= last; ++target) if (!coordinator.QueueRecoveredInput(expected.at(target), error)) return false;
			}
			if (!pair.Until([&] { return pair.host.GetStats().nextFrame == last + 1 && pair.client.GetStats().nextFrame == last + 1; }, error)) return false;
			const auto expectedInputs = [&](uint64_t first) {
				std::vector<NetLockstepFrame> result;
				for (uint64_t target = first; target <= last; ++target) {
					result.push_back(host.at(target)); result.push_back(client.at(target));
				}
				return result;
			};
			const auto capture = [&](uint64_t saved, NetResyncState& state) {
				NetResyncState captured;
				std::vector<uint8_t> bytes, archive;
				return ScenarioRunner::CaptureNetResyncState(saved, captured, error) &&
				       Check(captured.sourceRound == pair.host.GetRoundId() && SameInputs(captured.pendingInputs, expectedInputs(saved + 1)),
				             error, "delayed heal capture changed accepted input targets or bytes") &&
				       NetResyncCodec::Encode(captured, {0x44, 0x33}, bytes, error) &&
				       NetResyncCodec::Decode(bytes, captured.sessionId, saved + 1, state, archive, error) &&
				       Check(state == captured && SameInputs(state.pendingInputs, captured.pendingInputs) && archive == std::vector<uint8_t>({0x44, 0x33}),
				             error, "delayed heal envelope changed complete input state");
			};
			const auto restore = [&](const NetResyncState& state) {
				ScenarioRunner::SetLockstepCoordinator(nullptr);
				pair.hostConfig.startFrame = pair.clientConfig.startFrame = state.savedTick + 1;
				if (!RestartRound(pair, error)) return false;
				ScenarioRunner::SetLockstepCoordinator(&pair.host, true);
				for (auto* inputs: {&host, &client}) for (auto& [target, input]: *inputs) input.roundId = pair.host.GetRoundId();
				const auto expected = expectedInputs(state.savedTick + 1);
				const auto attempts = pair.hostTransport.recoveryAttempts + pair.clientTransport.recoveryAttempts;
				if (!ScenarioRunner::RestoreNetResyncState(state, error) || !pair.client.InstallResyncInputs(expected, error)) return false;
				std::vector<NetLockstepFrame> priming;
				for (uint16_t index = 0; index < clientDelay; ++index) priming.push_back(client.at(state.savedTick + 1 + index));
				if (!pair.client.PrimeResyncInputs(priming, error)) return false;
				for (uint64_t produced = state.savedTick + 1; produced + hostDelay <= last; ++produced) {
					if (!ScenarioRunner::QueueLockstepLocalControllerFrames(produced, {Input(1, produced + 999)}, error)) return false;
				}
				return Check(SameInputs(pair.host.CapturePendingInputs(state.savedTick), expected) && SameInputs(pair.client.CapturePendingInputs(state.savedTick), expected) &&
				             pair.host.GetStats().framePacketsSent == 0 && pair.client.GetStats().framePacketsSent == 0 &&
				             pair.hostTransport.recoveryAttempts + pair.clientTransport.recoveryAttempts == attempts,
				             error, "delayed restore or priming sampled fresh input, changed history or resent accepted input");
			};
			const auto consume = [&](uint64_t first, uint64_t end) {
				if (!pair.Until([&] { return pair.host.GetStats().nextFrame == last + 1 && pair.client.GetStats().nextFrame == last + 1; }, error)) return false;
				for (uint64_t target = first; target <= end; ++target) {
					for (uint8_t local: {uint8_t{1}, uint8_t{2}}) {
						auto& coordinator = local == 1 ? pair.host : pair.client;
						NetLockstepReadyFrame ready;
						if (!Check(coordinator.PopReadyFrame(ready) && ready.frame == target, error, "repeated delayed restore changed commitment order or target")) return false;
						for (uint8_t peer: {uint8_t{1}, uint8_t{2}}) {
							const auto& expected = (peer == 1 ? host : client).at(target);
							if (!Check(SameInput(ReadyInput(ready, peer, local, coordinator.GetRoundId()), expected), error,
							           "repeated delayed restore changed controller, command or observation bytes")) return false;
							if (local != 1) continue;
							for (const auto& command: expected.commands) {
								if (const auto* bindings = std::get_if<NetGamePlayerBindings>(&command.payload)) {
									ScenarioRunner::ObserveLockstepPlayerBindings(peer, target, *bindings);
								} else if (!Check(ScenarioRunner::ConsumeLockstepGameCommand(command) && !ScenarioRunner::ConsumeLockstepGameCommand(command),
								                  error, "repeated delayed restore skipped or reapplied a command")) return false;
							}
						}
					}
					(void)ScenarioRunner::FinishLockstepSimulationTick(target);
				}
				return true;
			};
			NetResyncState first, second;
			if (!capture(c_Start - 1, first) || !restore(first) || !consume(c_Start, c_Start + 1) ||
			    !capture(c_Start + 1, second) || !Check(second.appliedCommands == std::map<uint8_t, uint64_t>{{1, (c_Start + 1) * 3 + 1}, {2, (c_Start + 1) * 3 + 1}},
			                                          error, "second delayed heal lost the consumed prefix watermark") ||
			    !restore(second) || !consume(c_Start + 2, last)) return false;
			for (int index = 0; index < 4; ++index) pair.Step();
			NetLockstepReadyFrame extra;
			NetResyncState completed;
			return Check(pair.host.IsRunning() && pair.client.IsRunning() && !pair.host.PopReadyFrame(extra) && !pair.client.PopReadyFrame(extra) &&
			             pair.host.GetStats().nextFrame == last + 1 && pair.client.GetStats().nextFrame == last + 1 &&
			             pair.host.GetStats().observationsCarried == 0 && pair.client.GetStats().observationsCarried == 0 &&
			             pair.host.GetStats().observationsDropped == 0 && pair.client.GetStats().observationsDropped == 0,
			             error, "repeated delayed restore emitted an extra input or retimed sound observations") &&
			       ScenarioRunner::CaptureNetResyncState(last, completed, error) &&
			       Check(completed.pendingInputs.empty() && completed.pendingCommands.empty() && completed.pendingPlayerBindings.empty() &&
			             completed.appliedCommands == std::map<uint8_t, uint64_t>{{1, last * 3 + 1}, {2, last * 3 + 1}},
			             error, "repeated delayed restore retained completed input or lost final command watermarks");
		}

		bool TestLocalUnknownInput(std::string* error) {
			Pair pair(44204, 0, 0);
			if (!pair.Start(error) || !pair.Running(error) || !pair.client.PrimeResyncInputs({}, error)) return false;
			ResetScenario reset;
			ScenarioRunner::SetLockstepCoordinator(&pair.client);
			InputsByFrame host, client;
			for (uint64_t target = c_Start; target <= c_Start + 1; ++target) {
				client[target] = FullInput(2, target, pair.client.GetRoundId());
				if (!QueueFull(pair.client, client.at(target), error)) return false;
			}
			if (!Check(pair.host.CapturePendingInputs(c_Start - 1).empty(), error, "unknown-input fixture allowed the host to receive the client's queued input")) return false;
			ScenarioRunner::SetLockstepCoordinator(nullptr);
			if (!RestartRound(pair, error)) return false;
			ScenarioRunner::SetLockstepCoordinator(&pair.client, true);
			auto authoritative = SnapshotState(pair);
			if (!ScenarioRunner::RestoreNetResyncState(authoritative, error) || !pair.host.PrimeResyncInputs({}, error)) return false;
			NetResyncState beforeResend;
			if (!ScenarioRunner::CaptureNetResyncState(c_Start - 1, beforeResend, error)) return false;
			for (auto& [target, input]: client) input.roundId = pair.client.GetRoundId();
			if (!Check(SameInputs(beforeResend.pendingInputs, {client.at(c_Start), client.at(c_Start + 1)}) &&
			           pair.client.CapturePendingInputs(c_Start - 1).empty(), error, "unknown local history was lost or treated as host-accepted input")) return false;
			for (uint64_t target = c_Start; target <= c_Start + 1; ++target) {
				host[target] = FullInput(1, target, pair.host.GetRoundId());
				if (!ScenarioRunner::QueueLockstepLocalControllerFrames(target, {Input(2, target + 99)}, error) || !QueueFull(pair.host, host.at(target), error)) return false;
			}
			return CollectFull(pair, c_Start + 1, host, client, error);
		}

		bool TestProductionRequeue(std::string* error) {
			Pair pair(44189, 3, 0);
			if (!pair.Start(error) || !pair.Running(error)) return false;
			ResetScenario reset;
			ScenarioRunner::SetLockstepCoordinator(&pair.host);
			NetResyncState state;
			state.sessionId = pair.hostConfig.sessionId;
			state.sourceRound = pair.hostConfig.roundId - 1;
			state.savedTick = c_Start - 1;
			state.appliedCommands = {{1, 7}};
			state.pendingCommands = {{c_Start, Command(1, 8)}, {c_Start + 2, Command(1, 9)}, {c_Start + 5, Command(1, 10)}};
			CommandsByFrame hostCommands, clientCommands;
			for (uint64_t target = c_Start; target <= c_Start + 5; ++target) hostCommands[target] = {};
			for (const auto& pending: state.pendingCommands) hostCommands[pending.frame].push_back(pending.command);
			NetGameCommand fresh{1, NetGameSetTeamFunds{0, 777}};
			NetGameCommand sequenced = fresh;
			sequenced.sequence = 11;
			hostCommands[c_Start + 5].push_back(sequenced);
			for (uint64_t target = c_Start + 2; target <= c_Start + 5; ++target) {
				NetGamePlayerBindings binding;
				binding.players[0].active = true;
				binding.players[0].human = true;
				binding.players[0].team = 0;
				binding.players[0].controlledUID = static_cast<int64_t>(4000 + target);
				NetGameCommand command{1, binding};
				state.pendingPlayerBindings.push_back({target, command});
				hostCommands[target].push_back(command);
			}
			if (!ScenarioRunner::RestoreNetResyncState(state, error)) return false;
			ScenarioRunner::EnqueueLocalGameCommand(fresh);
			if (!pair.client.PrimeResyncFrames({}, error)) return false;
			for (uint64_t produced = c_Start; produced <= c_Start + 2; ++produced) {
				if (!ScenarioRunner::QueueLockstepLocalControllerFrames(produced, {Input(1, produced + 3)}, error)) return false;
			}
			for (uint64_t produced = c_Start; produced <= c_Start + 5; ++produced) {
				if (!QueueOrdinary(pair.client, 2, produced, clientCommands, error)) return false;
			}
			return Collect(pair, c_Start + 5, hostCommands, clientCommands, error);
		}

		bool TestScenarioLifetime(std::string* error) {
			Pair pair(44188, 0, 0);
			if (!pair.Start(error) || !pair.Running(error)) return false;
			ResetScenario reset;
			const NetGameCommand pending{1, NetGameSwitchControl{4242, 0, 1}};
			ScenarioRunner::SetLockstepCoordinator(&pair.host);
			ScenarioRunner::EnqueueLocalGameCommand(pending);
			ScenarioRunner::SetLockstepCoordinator(nullptr);
			ScenarioRunner::SetLockstepCoordinator(&pair.host);
			if (!Check(ScenarioRunner::DrainLocalGameCommands().empty(), error, "a rematch retained an unsequenced command")) return false;
			ScenarioRunner::EnqueueLocalGameCommand(pending);
			ScenarioRunner::SetLockstepControlOverride(4242, 2);
			ScenarioRunner::SetLockstepCoordinator(nullptr);
			ScenarioRunner::SetLockstepCoordinator(&pair.host, true);
			if (!Check(ScenarioRunner::DrainLocalGameCommands() == std::vector<NetGameCommand>{pending}, error, "same-session resync discarded a pending local command")) return false;
			NetResyncState state;
			state.sessionId = pair.hostConfig.sessionId;
			state.sourceRound = pair.hostConfig.roundId - 1;
			state.savedTick = c_Start - 1;
			state.controlOwners = {{4242, 2}};
			state.droppedControlOwners = {{4344, 2}};
			state.appliedCommands = {{1, 7}, {2, 11}};
			ScenarioRunner::EnqueueLocalGameCommand(pending);
			if (!ScenarioRunner::RestoreNetResyncState(state, error)) return false;
			if (!Check(ScenarioRunner::DrainLocalGameCommands() == std::vector<NetGameCommand>{pending}, error, "snapshot restoration discarded an unsequenced local command")) return false;
			if (!Check(ScenarioRunner::GetLockstepActorOwner(4242, 0, false) == 2 &&
			           ScenarioRunner::GetLockstepDropTimeActorOwner(4344, 0, false) == 2 &&
			           ScenarioRunner::GetLockstepActorOwner(4344, 0, false) == pair.host.ResolveActorOwner(4344, 0, false),
			           error, "resync did not restore separate live and drop-time ownership maps")) return false;
			if (!Check(!ScenarioRunner::ConsumeLockstepGameCommand(Command(1, 7)) && ScenarioRunner::ConsumeLockstepGameCommand(Command(1, 8)) &&
			           !ScenarioRunner::ConsumeLockstepGameCommand(Command(1, 8)) && !ScenarioRunner::ConsumeLockstepGameCommand(Command(2, 11)) &&
			           ScenarioRunner::ConsumeLockstepGameCommand(Command(2, 12)), error, "resync lost per-sender applied command watermarks")) return false;
			for (bool changeSession: {true, false}) {
				ScenarioRunner::EnqueueLocalGameCommand(pending);
				ScenarioRunner::SetLockstepControlOverride(4242, 2);
				ScenarioRunner::SetLockstepCoordinator(nullptr);
				if (changeSession) pair.hostConfig.matchConfig.sessionId = ++pair.hostConfig.sessionId;
				else ++pair.hostConfig.seatPresenceEpoch[0];
				if (!pair.host.Start(pair.hostTransport, pair.hostConfig, error)) return false;
				ScenarioRunner::SetLockstepCoordinator(&pair.host, true);
				if (!Check(ScenarioRunner::DrainLocalGameCommands().empty() && ScenarioRunner::ConsumeLockstepGameCommand(Command(1, 7)) &&
				           ScenarioRunner::GetLockstepActorOwner(4242, 0, false) == pair.host.ResolveActorOwner(4242, 0, false) &&
				           ScenarioRunner::GetLockstepDropTimeActorOwner(4344, 0, false) == pair.host.ResolveActorOwnerBeforeLeaves(4344, 0, false),
				           error, "new session or epoch inherited commands, watermarks or ownership")) return false;
			}
			return true;
		}
	}

	int NetResyncRuntimeSelfTest::Run() {
		if (ScenarioRunner::HasLockstepCoordinator()) {
			std::cerr << "[net-resync-runtime-selftest] FAIL: an existing coordinator is attached" << std::endl;
			return 1;
		}
		size_t total = 0, failed = 0;
		const auto run = [&](const char* name, const std::function<bool(std::string*)>& test) {
			++total;
			std::string error;
			bool passed = false;
			try {
				ResetScenario reset;
				passed = test(&error);
			} catch (const std::exception& exception) {
				error = exception.what();
			}
			if (passed) std::cout << "[net-resync-runtime-selftest] PASS: " << name << std::endl;
			else { ++failed; std::cerr << "[net-resync-runtime-selftest] FAIL: " << name << ": " << error << std::endl; }
		};
		run("asymmetric priming 0/3", [](auto* error) { return TestAsymmetricPriming(0, 3, 44180, error); });
		run("asymmetric priming 3/0", [](auto* error) { return TestAsymmetricPriming(3, 0, 44181, error); });
		run("asymmetric priming 1/3", [](auto* error) { return TestAsymmetricPriming(1, 3, 44182, error); });
		run("asymmetric priming 3/1", [](auto* error) { return TestAsymmetricPriming(3, 1, 44183, error); });
		run("start mode", TestStartMode);
		run("lost start", TestLostStart);
		run("primed round readoption", TestPrimedRoundReadoption);
		run("production requeue", TestProductionRequeue);
		run("scenario lifetime", TestScenarioLifetime);
		run("binding ACK authority", TestBindingAckAuthority);
		run("checksum ACK authority", TestChecksumAckAuthority);
		run("snapshot capture", TestSnapshotCapture);
		run("full asymmetric input 0/3", [](auto* error) { return TestFullAsymmetric(0, 3, 44196, error); });
		run("full asymmetric input 3/0", [](auto* error) { return TestFullAsymmetric(3, 0, 44197, error); });
		run("full asymmetric input 1/3", [](auto* error) { return TestFullAsymmetric(1, 3, 44198, error); });
		run("full asymmetric input 3/1", [](auto* error) { return TestFullAsymmetric(3, 1, 44199, error); });
		run("recovery chunk retry", TestRecoveryChunkRetry);
		run("three-peer capture", TestThreePeerCapture);
		run("local history bound", TestLocalHistoryBound);
		run("second heal before resend", TestSecondHealBeforeResend);
		run("unknown local input", TestLocalUnknownInput);
		run("authoritative input restore", TestAuthoritativeInputRestore);
		run("restore metadata refusals", TestRestoreMetadataRefusals);
		run("restore history conflicts", TestRestoreHistoryConflicts);
		run("unsent intent targets", TestUnsentIntentTargets);
		run("repeated full restore 1/3", [](auto* error) { return TestRepeatedFullDelayedRestore(1, 3, 44213, error); });
		run("repeated full restore 3/1", [](auto* error) { return TestRepeatedFullDelayedRestore(3, 1, 44214, error); });
		if (failed != 0) {
			std::cerr << "[net-resync-runtime-selftest] FAIL: " << failed << " of " << total << " groups" << std::endl;
			return 1;
		}
		std::cout << "[net-resync-runtime-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
