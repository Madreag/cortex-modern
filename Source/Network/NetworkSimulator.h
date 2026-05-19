#pragma once

#include "Singleton.h"

#include <atomic>
#include <cstdint>

#define g_NetworkSimulator NetworkSimulator::Instance()

namespace RTE {

	/// Network simulator knobs. M0 is no-op (no transport exists yet).
	///
	/// MP M1 will plug actual packet injection into these knobs without touching call sites. The
	/// interface lives in M0 so the SettingsMan plumbing + observable surface is settled now and
	/// the MP M1 PR is smaller and isolated to the transport layer.
	class NetworkSimulator : public Singleton<NetworkSimulator> {
		friend class Singleton<NetworkSimulator>;

	public:
		NetworkSimulator();
		~NetworkSimulator();

		void Initialize() {}
		void Destroy() {}

		/// Set the simulated one-way latency in milliseconds.
		void SetLatencyMs(int ms) { m_LatencyMs.store(ms < 0 ? 0 : ms); }
		int  GetLatencyMs() const { return m_LatencyMs.load(); }

		/// Set the simulated packet-loss percentage (0..100).
		void SetLossPct(int pct) { m_LossPct.store(pct < 0 ? 0 : (pct > 100 ? 100 : pct)); }
		int  GetLossPct() const { return m_LossPct.load(); }

		/// Set the simulated jitter (random delay variance) in milliseconds.
		void SetJitterMs(int ms) { m_JitterMs.store(ms < 0 ? 0 : ms); }
		int  GetJitterMs() const { return m_JitterMs.load(); }

		/// Is the simulator effectively active? True if any knob is non-zero.
		bool IsActive() const {
			return m_LatencyMs.load() > 0 || m_LossPct.load() > 0 || m_JitterMs.load() > 0;
		}

	private:
		std::atomic<int> m_LatencyMs{0};
		std::atomic<int> m_LossPct{0};
		std::atomic<int> m_JitterMs{0};
	};

} // namespace RTE
