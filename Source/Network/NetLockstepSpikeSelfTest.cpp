#include "NetLockstepSpikeSelfTest.h"

#include "ControllerLog.h"
#include "LoopbackTransport.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace RTE {

	namespace {
		constexpr uint32_t c_BundleMagic = 0x314B534CU; // LSK1
		constexpr uint16_t c_BundleVersion = 1;
		constexpr uint64_t c_MaxTicks = 24;
		constexpr uint64_t c_InputDelayTicks = 3;
		constexpr uint64_t c_ResendTailTicks = 4;
		constexpr uint64_t c_StallThresholdMs = 40;

		struct SpikeCounters {
			uint64_t duplicateBundles = 0;
			uint64_t lateBundles = 0;
			uint64_t stallEvents = 0;
			uint64_t maxStallMs = 0;
			uint64_t tickHashMismatches = 0;
		};

		struct Bundle {
			uint32_t sourcePeerId = 0;
			uint64_t tick = 0;
			std::vector<ControllerFrame> frames;
		};

		struct Peer {
			uint32_t peerId = 0;
			LoopbackTransport* transport = nullptr;
			NetPeerId transportPeerId = c_InvalidNetPeerId;
			ControllerLog localLog;
			std::map<uint64_t, std::map<uint32_t, std::vector<ControllerFrame>>> framesByTick;
			std::map<uint64_t, uint32_t> tickHashes;
			uint64_t currentTick = 0;
			uint64_t lastProgressMs = 0;
			uint64_t highestSendableTick = 0;
			bool stalled = false;
		};

		void AppendU16LE(std::vector<uint8_t>& out, uint16_t value) {
			out.push_back(static_cast<uint8_t>(value & 0xFFU));
			out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
		}

		void AppendU32LE(std::vector<uint8_t>& out, uint32_t value) {
			for (int i = 0; i < 4; ++i) {
				out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFFU));
			}
		}

		void AppendU64LE(std::vector<uint8_t>& out, uint64_t value) {
			for (int i = 0; i < 8; ++i) {
				out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFFU));
			}
		}

		bool ReadU16LE(const std::vector<uint8_t>& bytes, size_t& offset, uint16_t& value) {
			if (offset > bytes.size() || bytes.size() - offset < 2U) {
				return false;
			}
			value = static_cast<uint16_t>(bytes[offset]) |
			        static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset + 1]) << 8);
			offset += 2;
			return true;
		}

		bool ReadU32LE(const std::vector<uint8_t>& bytes, size_t& offset, uint32_t& value) {
			if (offset > bytes.size() || bytes.size() - offset < 4U) {
				return false;
			}
			value = 0;
			for (int i = 0; i < 4; ++i) {
				value |= static_cast<uint32_t>(bytes[offset + i]) << (i * 8);
			}
			offset += 4;
			return true;
		}

		bool ReadU64LE(const std::vector<uint8_t>& bytes, size_t& offset, uint64_t& value) {
			if (offset > bytes.size() || bytes.size() - offset < 8U) {
				return false;
			}
			value = 0;
			for (int i = 0; i < 8; ++i) {
				value |= static_cast<uint64_t>(bytes[offset + i]) << (i * 8);
			}
			offset += 8;
			return true;
		}

		uint32_t MixHash(uint32_t hash, uint32_t value) {
			hash ^= value + 0x9E3779B9U + (hash << 6U) + (hash >> 2U);
			return hash;
		}

		uint32_t TickHash(const std::map<uint32_t, std::vector<ControllerFrame>>& framesByPeer) {
			uint32_t hash = 2166136261U;
			for (const auto& [peerId, frames] : framesByPeer) {
				hash = MixHash(hash, peerId);
				for (const ControllerFrame& frame : frames) {
					const std::vector<uint8_t> encoded = ControllerFrameCodec::Encode(frame);
					hash = MixHash(hash, ControllerFrameCodec::PayloadChecksum(encoded));
				}
			}
			return hash;
		}

		ControllerFrame MakeFrame(uint32_t peerId, uint64_t tick, int frameIndex) {
			ControllerFrame frame;
			frame.actorUniqueID = static_cast<int64_t>((peerId * 1000U) + static_cast<uint32_t>(frameIndex));
			frame.stateMask = (uint64_t{1} << ((tick + peerId + static_cast<uint32_t>(frameIndex)) % 16U));
			frame.analogMoveX = static_cast<int16_t>((static_cast<int>(tick) * 31) + static_cast<int>(peerId));
			frame.analogMoveY = static_cast<int16_t>((static_cast<int>(tick) * -17) - static_cast<int>(peerId));
			frame.analogAimX = static_cast<int16_t>(100 + static_cast<int>(tick));
			frame.analogAimY = static_cast<int16_t>(-100 - static_cast<int>(tick));
			frame.inputMode = static_cast<uint8_t>(Controller::CIM_PLAYER);
			frame.playerRaw = static_cast<int8_t>(peerId - 1U);
			return frame;
		}

		ControllerLog MakeLog(uint32_t peerId) {
			ControllerLog log;
			for (uint64_t tick = 0; tick < c_MaxTicks; ++tick) {
				std::vector<ControllerFrame> frames;
				frames.push_back(MakeFrame(peerId, tick, 0));
				frames.push_back(MakeFrame(peerId, tick, 1));
				log.AddTick(tick, std::move(frames));
			}
			return log;
		}

		bool EncodeBundle(const Bundle& bundle, std::vector<uint8_t>& out, std::string* error) {
			if (bundle.frames.size() > UINT16_MAX) {
				*error = "spike bundle has too many frames";
				return false;
			}
			out.clear();
			AppendU32LE(out, c_BundleMagic);
			AppendU16LE(out, c_BundleVersion);
			AppendU16LE(out, static_cast<uint16_t>(bundle.frames.size()));
			AppendU32LE(out, bundle.sourcePeerId);
			AppendU64LE(out, bundle.tick);
			for (const ControllerFrame& frame : bundle.frames) {
				const std::vector<uint8_t> encoded = ControllerFrameCodec::Encode(frame);
				out.insert(out.end(), encoded.begin(), encoded.end());
			}
			return true;
		}

		bool DecodeBundle(const std::vector<uint8_t>& bytes, Bundle& bundle, std::string* error) {
			size_t offset = 0;
			uint32_t magic = 0;
			uint16_t version = 0;
			uint16_t frameCount = 0;
			if (!ReadU32LE(bytes, offset, magic) || magic != c_BundleMagic ||
			    !ReadU16LE(bytes, offset, version) || version != c_BundleVersion ||
			    !ReadU16LE(bytes, offset, frameCount) ||
			    !ReadU32LE(bytes, offset, bundle.sourcePeerId) ||
			    !ReadU64LE(bytes, offset, bundle.tick)) {
				*error = "spike bundle header decode failed";
				return false;
			}
			const size_t expectedSize = offset + (static_cast<size_t>(frameCount) * ControllerFrame::c_EncodedSize);
			if (bytes.size() != expectedSize) {
				*error = "spike bundle size mismatch";
				return false;
			}
			bundle.frames.clear();
			bundle.frames.reserve(frameCount);
			for (uint16_t i = 0; i < frameCount; ++i) {
				ControllerFrame frame;
				std::string decodeError;
				if (!ControllerFrameCodec::Decode(bytes.data() + offset, ControllerFrame::c_EncodedSize, frame, &decodeError)) {
					*error = "spike bundle frame decode failed: " + decodeError;
					return false;
				}
				bundle.frames.push_back(frame);
				offset += ControllerFrame::c_EncodedSize;
			}
			return true;
		}

		bool QueueLocalFrame(Peer& peer, uint64_t tick, std::string* error) {
			if (peer.framesByTick[tick].find(peer.peerId) != peer.framesByTick[tick].end()) {
				return true;
			}
			const ControllerLogTick* rec = peer.localLog.FindTick(tick);
			if (!rec) {
				*error = "missing local log tick";
				return false;
			}
			peer.framesByTick[tick][peer.peerId] = rec->frames;
			return true;
		}

		bool SendTick(Peer& peer, uint64_t tick, std::string* error) {
			const ControllerLogTick* rec = peer.localLog.FindTick(tick);
			if (!rec) {
				*error = "missing send log tick";
				return false;
			}
			std::vector<uint8_t> bytes;
			if (!EncodeBundle({peer.peerId, tick, rec->frames}, bytes, error)) {
				return false;
			}
			return peer.transport->Send(peer.transportPeerId, NetTransportLane::InputUnreliable, bytes, error);
		}

		bool SendAvailableWindow(Peer& peer, uint64_t nowMs, std::string* error) {
			peer.highestSendableTick = std::min<uint64_t>(c_MaxTicks - 1U, nowMs + c_InputDelayTicks);
			const uint64_t firstTick = peer.currentTick > c_ResendTailTicks ? peer.currentTick - c_ResendTailTicks : 0;
			for (uint64_t tick = firstTick; tick <= peer.highestSendableTick; ++tick) {
				if (!QueueLocalFrame(peer, tick, error) || !SendTick(peer, tick, error)) {
					return false;
				}
			}
			return true;
		}

		bool ReceivePackets(Peer& peer, SpikeCounters& counters, std::string* error) {
			for (const NetTransportEvent& event : peer.transport->PollEvents()) {
				if (event.type != NetTransportEventType::PacketReceived) {
					continue;
				}
				Bundle bundle;
				if (!DecodeBundle(event.bytes, bundle, error)) {
					return false;
				}
				if (bundle.tick < peer.currentTick) {
					++counters.lateBundles;
					continue;
				}
				auto& peerFrames = peer.framesByTick[bundle.tick];
				if (peerFrames.find(bundle.sourcePeerId) != peerFrames.end()) {
					++counters.duplicateBundles;
					continue;
				}
				peerFrames[bundle.sourcePeerId] = std::move(bundle.frames);
			}
			return true;
		}

		void TryAdvance(Peer& peer, uint64_t nowMs, SpikeCounters& counters) {
			while (peer.currentTick < c_MaxTicks) {
				auto framesIt = peer.framesByTick.find(peer.currentTick);
				if (framesIt == peer.framesByTick.end() || framesIt->second.size() < 2U) {
					break;
				}
				peer.tickHashes[peer.currentTick] = TickHash(framesIt->second);
				++peer.currentTick;
				peer.lastProgressMs = nowMs;
				peer.stalled = false;
			}
			const uint64_t stalledMs = nowMs >= peer.lastProgressMs ? nowMs - peer.lastProgressMs : 0;
			counters.maxStallMs = std::max(counters.maxStallMs, stalledMs);
			if (!peer.stalled && peer.currentTick < c_MaxTicks && stalledMs >= c_StallThresholdMs) {
				peer.stalled = true;
				++counters.stallEvents;
			}
		}

		bool CompareTickHashes(const Peer& hostPeer, const Peer& clientPeer, SpikeCounters& counters, std::string* error) {
			for (uint64_t tick = 0; tick < c_MaxTicks; ++tick) {
				const auto hostHash = hostPeer.tickHashes.find(tick);
				const auto clientHash = clientPeer.tickHashes.find(tick);
				if (hostHash == hostPeer.tickHashes.end() || clientHash == clientPeer.tickHashes.end()) {
					*error = "missing spike tick hash";
					return false;
				}
				if (hostHash->second != clientHash->second) {
					++counters.tickHashMismatches;
					*error = "spike tick hash mismatch at tick " + std::to_string(tick);
					return false;
				}
			}
			return true;
		}

		bool RunCoordinator(const LoopbackTransportConfig& hostFaults,
		                    const LoopbackTransportConfig& clientFaults,
		                    bool expectComplete,
		                    SpikeCounters& counters,
		                    std::string* error) {
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			hostTransport.SetFaultConfig(hostFaults);
			clientTransport.SetFaultConfig(clientFaults);
			if (!hostTransport.StartHost(42001, error) || !clientTransport.Connect("loopback", 42001, error)) {
				return false;
			}
			hostTransport.PollEvents();
			clientTransport.PollEvents();

			Peer hostPeer{1, &hostTransport, 1, MakeLog(1)};
			Peer clientPeer{2, &clientTransport, 1, MakeLog(2)};

			for (uint64_t nowMs = 0; nowMs < 400 && (hostPeer.currentTick < c_MaxTicks || clientPeer.currentTick < c_MaxTicks); ++nowMs) {
				if (!SendAvailableWindow(hostPeer, nowMs, error) || !SendAvailableWindow(clientPeer, nowMs, error)) {
					return false;
				}
				if (!ReceivePackets(hostPeer, counters, error) || !ReceivePackets(clientPeer, counters, error)) {
					return false;
				}
				TryAdvance(hostPeer, nowMs, counters);
				TryAdvance(clientPeer, nowMs, counters);
				hostTransport.AdvanceTimeMs(1);
				clientTransport.AdvanceTimeMs(1);
			}

			const bool completed = hostPeer.currentTick == c_MaxTicks && clientPeer.currentTick == c_MaxTicks;
			if (completed != expectComplete) {
				*error = expectComplete ? "spike coordinator did not complete" : "spike coordinator completed unexpectedly";
				return false;
			}
			if (!expectComplete) {
				return counters.stallEvents > 0;
			}
			return CompareTickHashes(hostPeer, clientPeer, counters, error);
		}

		bool TestNoFaults(std::string* error) {
			SpikeCounters counters;
			LoopbackTransportConfig noFaults;
			if (!RunCoordinator(noFaults, noFaults, true, counters, error)) {
				return false;
			}
			if (counters.tickHashMismatches != 0 || counters.stallEvents != 0) {
				*error = "no-fault coordinator reported unexpected mismatch or stall";
				return false;
			}
			return true;
		}

		bool TestFaults(std::string* error) {
			SpikeCounters counters;
			LoopbackTransportConfig hostFaults;
			hostFaults.latencyMs = 4;
			hostFaults.jitterMs = 3;
			hostFaults.reorderUnreliable = true;
			hostFaults.unreliableDropEveryN = 5;
			hostFaults.unreliableDuplicateEveryN = 7;
			LoopbackTransportConfig clientFaults = hostFaults;
			clientFaults.unreliableDropEveryN = 6;
			clientFaults.unreliableDuplicateEveryN = 4;
			if (!RunCoordinator(hostFaults, clientFaults, true, counters, error)) {
				return false;
			}
			if (counters.duplicateBundles == 0 || counters.lateBundles == 0) {
				*error = "fault coordinator did not exercise duplicate and late rejection";
				return false;
			}
			return true;
		}

		bool TestPermanentLossStalls(std::string* error) {
			SpikeCounters counters;
			LoopbackTransportConfig dropAll;
			dropAll.unreliableDropEveryN = 1;
			if (!RunCoordinator(dropAll, dropAll, false, counters, error)) {
				return false;
			}
			if (counters.stallEvents == 0 || counters.maxStallMs < c_StallThresholdMs) {
				*error = "permanent loss did not produce stall diagnostics";
				return false;
			}
			return true;
		}
	}

	int NetLockstepSpikeSelfTest::Run() {
		auto fail = [](const std::string& message) {
			std::cerr << "[net-lockstep-spike-selftest] FAIL: " << message << std::endl;
			return 1;
		};

		std::string error;
		if (!TestNoFaults(&error)) {
			return fail(error);
		}
		if (!TestFaults(&error)) {
			return fail(error);
		}
		if (!TestPermanentLossStalls(&error)) {
			return fail(error);
		}

		std::cout << "[net-lockstep-spike-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
