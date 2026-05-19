#pragma once

#include "Singleton.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#define g_ReplayLog ReplayLog::Instance()

namespace RTE {

	/// Records the input stream of a sim run to a binary `.replay` file.
	///
	/// Format (little-endian):
	///   Header (40 bytes):
	///     [0..4)    magic        = "CCRP" (Cortex Command ReplaY)
	///     [4..8)    version      = 1
	///     [8..16)   seed         = uint64
	///     [16..24)  tick_count   = uint64
	///     [24..40)  scenario     = char[16] (null-padded, ASCII)
	///   Per-tick record (variable):
	///     [0..8)    tick         = uint64
	///     [8..12)   player_count = uint32
	///     For each player:
	///       [0..4)  player_id    = int32
	///       [4..N)  controller_state = packed bytes
	///
	/// At M0 we record only; offline replay-verify can stretch into M5.
	class ReplayLog : public Singleton<ReplayLog> {
		friend class Singleton<ReplayLog>;

	public:
		struct PlayerInput {
			int32_t              player_id = -1;
			std::vector<uint8_t> state;
		};

		ReplayLog();
		~ReplayLog();

		void Initialize() {}
		void Destroy();

		/// Begin recording. Resets buffer.
		void BeginRecording(const std::string& scenario, uint64_t seed);

		/// Append a tick's inputs.
		void RecordTick(uint64_t tick, const std::vector<PlayerInput>& inputs);

		/// Stop recording.
		void EndRecording();

		/// Write the recorded log to a binary file. Returns true on success.
		bool Write(const std::string& path) const;

		/// Get the number of recorded ticks (for diagnostics).
		uint64_t GetTickCount() const {
			std::lock_guard<std::mutex> lock(m_Mutex);
			return static_cast<uint64_t>(m_Ticks.size());
		}

		/// Are we currently recording?
		bool IsRecording() const {
			std::lock_guard<std::mutex> lock(m_Mutex);
			return m_Recording;
		}

	private:
		struct TickRec {
			uint64_t                 tick = 0;
			std::vector<PlayerInput> inputs;
		};

		mutable std::mutex   m_Mutex;
		bool                 m_Recording = false;
		std::string          m_Scenario;
		uint64_t             m_Seed = 0;
		std::vector<TickRec> m_Ticks;
	};

} // namespace RTE
