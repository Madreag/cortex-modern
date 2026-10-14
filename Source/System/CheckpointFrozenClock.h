#pragma once

#include "NoInline.h"

#include <atomic>

namespace RTE {
	struct CheckpointFrozenClock {
		long long simTicks = 0, simUpdates = 0, realTicks = 0;
		/// The calling thread's frozen clock, if a scope holds one; while no thread holds one, the answer reads no thread-local.
		static const CheckpointFrozenClock* Current() noexcept { return s_Scopes.load(std::memory_order_relaxed) != 0 ? CurrentHere() : nullptr; }
		class Scope {
		public:
			explicit Scope(const CheckpointFrozenClock* clock) : m_Previous(current), m_Counted(clock != nullptr) {
				if (!clock) return;
				s_Scopes.fetch_add(1, std::memory_order_relaxed);
				current = clock;
			}
			~Scope() {
				current = m_Previous;
				if (m_Counted) s_Scopes.fetch_sub(1, std::memory_order_relaxed);
			}
		private:
			const CheckpointFrozenClock* m_Previous;
			bool m_Counted;
		};
	private:
		RTE_NOINLINE static const CheckpointFrozenClock* CurrentHere() noexcept;
		inline static thread_local const CheckpointFrozenClock* current = nullptr;
		inline static std::atomic<int> s_Scopes{0}; //!< Scopes holding a clock, on all threads.
	};
}
