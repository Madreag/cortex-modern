#pragma once

namespace RTE {
	struct CheckpointFrozenClock {
		long long simTicks = 0, simUpdates = 0, realTicks = 0;
		inline static thread_local const CheckpointFrozenClock* current = nullptr;
		class Scope {
		public:
			explicit Scope(const CheckpointFrozenClock* clock) : m_Previous(current) { if (clock) current = clock; }
			~Scope() { current = m_Previous; }
		private:
			const CheckpointFrozenClock* m_Previous;
		};
	};
}
