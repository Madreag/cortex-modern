#pragma once

#include <atomic>

namespace RTE {

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define RTE_CAPTURE_SENTINEL_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(RTE_CAPTURE_SENTINEL_ASAN)
#define RTE_CAPTURE_SENTINEL_ASAN 1
#endif

	/// Names the engine objects made on a checkpoint capture's worker threads. The workers only read the world while
	/// the capturing thread waits, so anything they make races the other workers and that thread.
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

		/// Whether creations are checked: from the start in Debug and ASan builds, after Enable() (-checkpoint-sentinel) in Final.
		static bool Enabled() { return s_Enabled.load(std::memory_order_relaxed); }

		/// Turns the checks on and says so once.
		static void Enable();

		/// Reports an engine object of the named type made on a capture thread; costs one thread-local read elsewhere.
		/// @param type The class whose constructor or creator calls this.
		/// @param object The object being made, so a derived constructor names the type a base's report started.
		static void NoteCreation(const char* type, const void* object) {
			if (s_Task && Enabled()) Report(type, object);
		}

		/// How many engine objects were reported made on a capture thread this run.
		static int HitCount() { return s_Hits.load(std::memory_order_acquire); }

	private:
		static void Report(const char* type, const void* object);
		static void Flush();

#if defined(DEBUGMODE) || defined(DEBUG_BUILD) || defined(MIN_DEBUG_BUILD) || defined(RTE_CAPTURE_SENTINEL_ASAN)
		static constexpr bool c_DefaultEnabled = true;
#else
		static constexpr bool c_DefaultEnabled = false;
#endif
		inline static thread_local const char* s_Task = nullptr;
		inline static std::atomic<bool> s_Enabled{c_DefaultEnabled};
		inline static std::atomic<int> s_ParallelPhases{0};
		inline static std::atomic<int> s_Hits{0};
	};
} // namespace RTE
