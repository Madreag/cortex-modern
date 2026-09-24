#pragma once

#include <atomic>

namespace RTE {

	/// Marks the threads a checkpoint capture runs in parallel. The workers only read the world while the capturing
	/// thread waits, so anything they make races the other workers and that thread.
	class CaptureSentinel {
	public:
		/// Marks this thread as one of a capture's parallel workers while it lives.
		class WorkerScope {
		public:
			/// @param task What the worker runs, named in a report.
			explicit WorkerScope(const char* task) : m_Previous(s_Task) { s_Task = task; }
			~WorkerScope();
			WorkerScope(const WorkerScope&) = delete;
			WorkerScope& operator=(const WorkerScope&) = delete;

		private:
			const char* m_Previous;
		};

		/// Held by the capturing thread from the moment the first worker may run until the last one is joined.
		class ParallelPhase {
		public:
			ParallelPhase() { s_ParallelPhases.fetch_add(1, std::memory_order_acq_rel); }
			~ParallelPhase() { s_ParallelPhases.fetch_sub(1, std::memory_order_acq_rel); }
			ParallelPhase(const ParallelPhase&) = delete;
			ParallelPhase& operator=(const ParallelPhase&) = delete;
		};

		/// Whether this thread runs part of a capture's parallel phase.
		static bool OnCaptureThread() { return s_Task != nullptr; }

		/// The capture task this thread runs, or null; a worker it hands part of that task to carries the same one.
		static const char* CurrentTask() { return s_Task; }

		/// Whether a capture's workers may be running now, on any thread.
		static bool InParallelPhase() { return s_ParallelPhases.load(std::memory_order_acquire) > 0; }

	private:
		inline static thread_local const char* s_Task = nullptr;
		inline static std::atomic<int> s_ParallelPhases{0};
	};
} // namespace RTE
