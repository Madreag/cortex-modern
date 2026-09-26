#include "NetCommittedTailRing.h"

#include "LoopbackTransport.h"
#include "NetWorldJoin.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>

namespace RTE {

	size_t NetCommittedTailRing::CapacityFrames(uint32_t boundTicks, uint32_t delayMarginFrames, uint64_t captureIntervalMs, double tickMs) {
		const double tick = std::isfinite(tickMs) && tickMs > 0 ? tickMs : 1000.0 / 60.0;
		return static_cast<size_t>(boundTicks) + delayMarginFrames + static_cast<size_t>(std::ceil(static_cast<double>(captureIntervalMs) / tick));
	}

	void NetCommittedTailRing::Configure(uint64_t roundId, size_t capacityFrames) {
		Clear();
		m_RoundId = roundId;
		m_Capacity = capacityFrames;
	}

	bool NetCommittedTailRing::Append(const NetLockstepFrame& frame, std::string* error) {
		if (!IsConfigured() || frame.roundId != m_RoundId) {
			if (error) *error = "the committed tail ring is not recording round " + std::to_string(frame.roundId);
			return false;
		}
		Record record;
		record.frame = frame.targetFrame;
		if (!EncodeCommittedJoinFrame(frame, record.bytes, error)) return false;
		if (!m_Records.empty() && record.frame != m_Records.back().frame + 1) {
			m_Records.clear();
			m_Bytes = 0;
			++m_Restarts;
		}
		m_Bytes += record.bytes.size();
		m_Records.push_back(std::move(record));
		while (m_Records.size() > m_Capacity) {
			m_Bytes -= m_Records.front().bytes.size();
			m_Records.pop_front();
		}
		return true;
	}

	bool NetCommittedTailRing::Serve(uint64_t from, std::vector<std::vector<uint8_t>>& out, uint64_t* firstServable) const {
		out.clear();
		if (firstServable) *firstServable = FirstFrame();
		if (m_Records.empty() || from < m_Records.front().frame) return false;
		for (auto record = m_Records.begin() + static_cast<std::ptrdiff_t>(std::min<uint64_t>(from - m_Records.front().frame, m_Records.size())); record != m_Records.end(); ++record) {
			out.push_back(record->bytes);
		}
		return true;
	}

	void NetCommittedTailRing::Clear() {
		m_Records.clear();
		m_RoundId = 0;
		m_Capacity = 0;
		m_Bytes = 0;
		m_Restarts = 0;
	}

	bool NetCommittedTailRing::SelfTest(std::string* error) {
		const auto fail = [&](const std::string& message) { if (error) *error = message; return false; };
		const uint16_t port = 48976;
		LoopbackTransport hostWire, clientAWire, clientBWire;
		std::string wireError;
		if (!hostWire.StartHost(port, &wireError) || !clientAWire.Connect("loopback", port, &wireError) || !clientBWire.Connect("loopback", port, &wireError)) return fail(wireError);
		const auto config = [](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
			NetLockstepConfig c;
			c.sessionId = 0x9A76; c.roundId = 0x9A76; c.startFrame = 0; c.inputDelayFrames = 4; c.timeoutMs = 1000000;
			c.localPeerId = local; c.peerCount = 3; c.remoteTransportPeerIds = std::move(transports); c.relayToOtherPeers = relay;
			c.scenario = "LockstepSelfTest"; c.ownershipPolicy = "unique-id-split"; c.simTickMs = 1000.0 / 60.0;
			return c;
		};
		NetLockstepCoordinator host, clientA, clientB;
		if (!host.Start(hostWire, config(1, {{2, 1}, {3, 2}}, true), &wireError) || !clientA.Start(clientAWire, config(2, {{1, 1}}, false), &wireError) ||
		    !clientB.Start(clientBWire, config(3, {{1, 1}}, false), &wireError)) return fail(wireError);
		const size_t capacity = CapacityFrames(3, 4, 10000, 1000.0 / 60.0);
		if (capacity != 607) return fail("a 60 Hz ring with a 3-tick bound and a 4-frame margin keeps " + std::to_string(capacity) + " frames, not 607");
		// The host's record is the join plane's own tail; each client keeps a ring from its own coordinator's commits.
		NetWorldFrameLog hostRecord;
		NetCommittedTailRing rings[2];
		for (NetCommittedTailRing& ring: rings) ring.Configure(0x9A76, capacity);
		struct Peer { NetLockstepCoordinator* coordinator; LoopbackTransport* wire; uint8_t id; uint64_t simulated = 0, queued = 0; };
		Peer peers[3] = {{&host, &hostWire, 1}, {&clientA, &clientAWire, 2}, {&clientB, &clientBWire, 3}};
		constexpr uint64_t c_Frames = 720;
		std::string recordError;
		bool recorded = true;
		for (uint64_t now = 0; now < 60000 && std::any_of(std::begin(peers), std::end(peers), [&](const Peer& peer) { return peer.simulated < c_Frames; }); ++now) {
			for (Peer& peer: peers) {
				for (; peer.queued <= peer.simulated + 6; ++peer.queued) {
					ControllerFrame input;
					input.actorUniqueID = 100 * peer.id; input.stateMask = peer.queued; input.inputMode = static_cast<uint8_t>(Controller::CIM_PLAYER);
					// Every seat sends a command every frame, so a frame's commands come from three senders in each peer's own local-first order.
					const NetGameCommand command{peer.id, NetGameSetTeamFunds{static_cast<uint8_t>(peer.id - 1), static_cast<int32_t>(peer.queued)}, peer.queued};
					if (!peer.coordinator->IsRunning() || !peer.coordinator->QueueLocalInput(peer.queued, {input}, {command}, &recordError)) break;
				}
				peer.wire->AdvanceTimeMs(1);
				peer.coordinator->Tick(now);
			}
			for (size_t index = 0; index < 3; ++index) {
				for (NetLockstepReadyFrame ready; peers[index].coordinator->PopReadyFrame(ready);) {
					(void)peers[index].coordinator->FinishSimulationTick(ready.frame);
					peers[index].simulated = ready.frame;
					auto frame = PackWorldJoinReadyFrame(ready); frame.roundId = peers[index].coordinator->GetRoundId();
					recorded = recorded && (index == 0 ? hostRecord.Append(frame, &recordError) : rings[index - 1].Append(frame, &recordError));
				}
			}
		}
		if (!recorded) return fail("a committed frame did not enter a record: " + recordError);
		if (std::any_of(std::begin(peers), std::end(peers), [&](const Peer& peer) { return peer.simulated < c_Frames; }))
			return fail("the three-peer round stopped short: frames " + std::to_string(peers[0].simulated) + "/" + std::to_string(peers[1].simulated) + "/" + std::to_string(peers[2].simulated));
		for (const NetCommittedTailRing& ring: rings) {
			// Bounded: the ring holds its capacity and no more, the newest frames, in commit order.
			if (ring.Count() != capacity || ring.LastFrame() < c_Frames || ring.FirstFrame() != ring.LastFrame() - capacity + 1 || ring.Restarts() != 0)
				return fail("the ring is not the newest " + std::to_string(capacity) + " frames: count=" + std::to_string(ring.Count()) + " first=" + std::to_string(ring.FirstFrame()) +
				            " last=" + std::to_string(ring.LastFrame()) + " restarts=" + std::to_string(ring.Restarts()));
			const uint64_t from = ring.LastFrame() - 599;
			std::vector<std::vector<uint8_t>> served, record;
			uint64_t first = 0, recordLast = 0;
			if (!ring.Serve(from, served, &first) || served.size() != 600) return fail("the ring did not serve the 600 frames from " + std::to_string(from));
			(void)hostRecord.CopyFrom(from, 600, UINT64_MAX, record, &recordLast);
			if (record.size() != 600 || recordLast != from + 599) return fail("the host's record does not cover the 600 frames from " + std::to_string(from));
			for (size_t index = 0; index < served.size(); ++index) {
				NetLockstepFrame decoded;
				if (!DecodeCommittedJoinFrame(served[index], decoded, &recordError) || decoded.targetFrame != from + index)
					return fail("the served tail is out of commit order at " + std::to_string(from + index) + ": " + recordError);
				if (served[index] != record[index]) {
					NetLockstepFrame hosted;
					(void)DecodeCommittedJoinFrame(record[index], hosted, nullptr);
					return fail("the served tail differs from the host's record at frame " + std::to_string(from + index) + ": commands " + std::to_string(decoded.commands.size()) +
					            "/" + std::to_string(hosted.commands.size()) + ", first sender " + std::to_string(decoded.commands.empty() ? 0 : decoded.commands.front().senderPeerId) +
					            "/" + std::to_string(hosted.commands.empty() ? 0 : hosted.commands.front().senderPeerId));
				}
			}
			// Past the ring: refused, naming the first frame it can serve.
			if (ring.Serve(ring.FirstFrame() - 1, served, &first) || first != ring.FirstFrame() || !served.empty())
				return fail("a frame older than the ring was served or its refusal named " + std::to_string(first) + " instead of " + std::to_string(ring.FirstFrame()));
		}
		// A gap restarts the ring at the new frame, so it never serves a run the round did not commit.
		NetCommittedTailRing gap = rings[0];
		NetLockstepFrame skipped;
		skipped.targetFrame = gap.LastFrame() + 2; skipped.roundId = 0x9A76;
		std::vector<std::vector<uint8_t>> served;
		uint64_t first = 0;
		if (!gap.Append(skipped, &recordError) || gap.Count() != 1 || gap.Restarts() != 1 || gap.Serve(skipped.targetFrame - 1, served, &first) || first != skipped.targetFrame)
			return fail("a gap in the commits did not restart the ring at the new frame");
		skipped.roundId = 0x9A77;
		if (gap.Append(skipped, nullptr)) return fail("the ring recorded a frame of another round");
		std::cout << "[net-match-selftest] PASS the_committed_tail_ring_serves_the_hosts_record capacity=" << capacity << " served=600 from=" << rings[0].LastFrame() - 599
		          << " refused_before=" << rings[0].FirstFrame() << std::endl;
		return true;
	}

} // namespace RTE
