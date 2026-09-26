#pragma once

#include "NetLockstep.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace RTE {

	/// Every peer's own record of the round's latest committed frames, each encoded exactly as the host's catch-up tail carries it.
	/// A successor serves a held seat's catch-up from it at a handover, so the seat keeps its world. It is never on the wire except
	/// as that served tail, so it changes no wire constant and no hash.
	class NetCommittedTailRing {
	public:
		/// The frames a ring keeps: the slow-player bound, the delay margin and one capture interval of frames.
		static size_t CapacityFrames(uint32_t boundTicks, uint32_t delayMarginFrames, uint64_t captureIntervalMs, double tickMs);

		/// Starts the ring for a round, empty. A ring for another round is dropped.
		void Configure(uint64_t roundId, size_t capacityFrames);
		bool IsConfigured() const { return m_RoundId != 0 && m_Capacity != 0; }
		uint64_t RoundId() const { return m_RoundId; }
		size_t Capacity() const { return m_Capacity; }

		/// Records one committed frame of the configured round in commit order. A frame that does not follow the last one restarts
		/// the ring at it: the ring only ever holds an unbroken run, so what it serves is exactly what the round committed.
		bool Append(const NetLockstepFrame& frame, std::string* error = nullptr);

		uint64_t FirstFrame() const { return m_Records.empty() ? 0 : m_Records.front().frame; }
		uint64_t LastFrame() const { return m_Records.empty() ? 0 : m_Records.back().frame; }
		size_t Count() const { return m_Records.size(); }
		uint64_t Bytes() const { return m_Bytes; }
		uint64_t Restarts() const { return m_Restarts; }
		bool Covers(uint64_t frame) const { return !m_Records.empty() && frame >= m_Records.front().frame && frame <= m_Records.back().frame; }

		/// The encoded frames from `from` through the last, in commit order. A `from` older than the ring is refused with the first
		/// frame the ring can serve; a `from` past the last frame serves nothing.
		bool Serve(uint64_t from, std::vector<std::vector<uint8_t>>& out, uint64_t* firstServable = nullptr) const;

		void Clear();

		/// Bounded size, commit order, the served tail equal to the host's record over 600 frames of a three-peer round, the refusal
		/// past the ring naming its first frame.
		static bool SelfTest(std::string* error);

	private:
		struct Record {
			uint64_t frame = 0;
			std::vector<uint8_t> bytes;
		};

		std::deque<Record> m_Records;
		uint64_t m_RoundId = 0;
		size_t m_Capacity = 0;
		uint64_t m_Bytes = 0;
		uint64_t m_Restarts = 0;
	};

} // namespace RTE
