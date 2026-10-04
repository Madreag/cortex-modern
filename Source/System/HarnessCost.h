#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace RTE {

	/// What the test instruments cost the frames a run simulates: each instrument's own work, measured where it runs - on the
	/// simulation thread or a worker - and charged to the frame being simulated when that work ends, for the per-frame receipts a
	/// run's instrument verdict reads. A sample that suspends the simulation thread is taken out of the simulation-thread work it
	/// interrupted, so no time is charged twice.
	class HarnessCost {

	public:
		enum Instrument : uint8_t { SimDump, TickEnd, FullState, Census, PreviewFidelity, StallSampler, ScreenWatches, Recorder, InstrumentCount };
		static constexpr std::array<const char*, InstrumentCount> c_Names{"sim_dump", "tick_end", "fullstate", "census", "preview_fidelity",
		                                                                 "stall_sampler", "screen_watches", "recorder"};

		/// Whether this process runs an instrument; the run's receipt names every instrument with its state.
		static void SetEnabled(Instrument instrument, bool enabled) { s_Enabled[instrument].store(enabled, std::memory_order_relaxed); }
		static bool Enabled(Instrument instrument) { return s_Enabled[instrument].load(std::memory_order_relaxed); }
		static bool AnyEnabled() {
			for (const auto& enabled: s_Enabled) {
				if (enabled.load(std::memory_order_relaxed)) {
					return true;
				}
			}
			return false;
		}

		/// Charges measured work to the frame being simulated.
		static void Charge(Instrument instrument, int64_t nanoseconds) {
			if (nanoseconds > 0) {
				s_Charged[instrument].fetch_add(nanoseconds, std::memory_order_relaxed);
			}
		}

		/// The time the simulation thread spent suspended for a stack sample: charged to the sampler, and taken out of the
		/// simulation-thread measurement it fell inside.
		static void NoteSimulationSuspended(int64_t nanoseconds) {
			Charge(StallSampler, nanoseconds);
			s_SuspendedNs.fetch_add(nanoseconds, std::memory_order_relaxed);
		}

		/// Measures simulation-thread work between its start and Stop, less any suspension for a stack sample inside it.
		class SimulationSpan {
		public:
			SimulationSpan() : m_Start(std::chrono::steady_clock::now()), m_Suspended(s_SuspendedNs.load(std::memory_order_relaxed)) {}
			/// The span's own nanoseconds.
			int64_t Stop() const {
				const int64_t elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_Start).count();
				return elapsed - (s_SuspendedNs.load(std::memory_order_relaxed) - m_Suspended);
			}
		private:
			std::chrono::steady_clock::time_point m_Start;
			int64_t m_Suspended;
		};

		/// Takes what the frame was charged, instrument by instrument, in nanoseconds.
		static std::array<int64_t, InstrumentCount> TakeFrame() {
			std::array<int64_t, InstrumentCount> frame{};
			for (size_t instrument = 0; instrument < InstrumentCount; ++instrument) {
				frame[instrument] = s_Charged[instrument].exchange(0, std::memory_order_relaxed);
			}
			return frame;
		}

	private:
		static inline std::array<std::atomic<bool>, InstrumentCount> s_Enabled{};
		static inline std::array<std::atomic<int64_t>, InstrumentCount> s_Charged{};
		static inline std::atomic<int64_t> s_SuspendedNs{0};
	};
} // namespace RTE
