#pragma once

/// Header file for the ThreadMan class.
/// Author(s):
/// Inclusions of header files
#include "Singleton.h"
#define g_ThreadMan ThreadMan::Instance()

#include "BS_thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>

namespace RTE {

	/// Runs work(index) for every index below a count on a pool, the items taken in index order by whichever thread comes
	/// free first. The owner takes items too when it finishes, so it never waits behind other work queued on the pool for
	/// an item nobody has started; it waits only for the items already running.
	class ParallelWork {
	public:
		/// @param pool The pool whose threads help. @param count How many items. @param work The item's work; it outlives
		/// no Finish, so it may hold references to the owner's locals. @param helpers At most this many pool threads help.
		ParallelWork(BS::thread_pool& pool, size_t count, std::function<void(size_t)> work, size_t helpers = DefaultHelpers()) :
		    m_Shared(std::make_shared<Shared>()) {
			m_Shared->count = count;
			m_Shared->work = std::move(work);
			const size_t threads = std::min<size_t>({count > 0 ? count - 1 : 0, helpers, static_cast<size_t>(pool.get_thread_count())});
			for (size_t helper = 0; helper < threads; ++helper) {
				pool.push_task([shared = m_Shared] { Run(*shared); });
			}
		}
		~ParallelWork() {
			if (!m_Finished) Finish(false);
		}
		ParallelWork(const ParallelWork&) = delete;
		ParallelWork& operator=(const ParallelWork&) = delete;

		/// Takes every item nobody started, waits for the ones running, and rethrows the first failure.
		void Finish(bool rethrow = true) {
			m_Finished = true;
			Shared& shared = *m_Shared;
			Run(shared);
			std::unique_lock lock(shared.mutex);
			shared.done.wait(lock, [&shared] { return shared.completed == shared.count; });
			// A helper that starts after this finds no item left and never calls the work.
			if (rethrow && shared.failure) std::rethrow_exception(shared.failure);
		}

		/// How many pool threads help by default: all, or CCCP_PARALLEL_HELPERS when the environment sets it.
		static size_t DefaultHelpers() {
			static const size_t helpers = [] {
				const char* value = std::getenv("CCCP_PARALLEL_HELPERS");
				const long parsed = value ? std::strtol(value, nullptr, 10) : 0;
				return parsed > 0 ? static_cast<size_t>(parsed) : static_cast<size_t>(-1);
			}();
			return helpers;
		}

	private:
		struct Shared {
			size_t count = 0;
			std::function<void(size_t)> work;
			std::atomic<size_t> next{0};
			std::mutex mutex;
			std::condition_variable done;
			size_t completed = 0;
			std::exception_ptr failure;
		};

		static void Run(Shared& shared) {
			for (size_t index = shared.next.fetch_add(1, std::memory_order_relaxed); index < shared.count; index = shared.next.fetch_add(1, std::memory_order_relaxed)) {
				std::exception_ptr failure;
				try {
					shared.work(index);
				} catch (...) {
					failure = std::current_exception();
				}
				std::lock_guard lock(shared.mutex);
				if (failure && !shared.failure) shared.failure = failure;
				if (++shared.completed == shared.count) shared.done.notify_all();
			}
		}

		std::shared_ptr<Shared> m_Shared;
		bool m_Finished = false;
	};

	/// The centralized singleton manager of all threads.
	class ThreadMan :
	    public Singleton<ThreadMan> {

		/// Public member variable, method and friend function declarations
	public:
		/// Constructor method used to instantiate a ThreadMan object in system
		/// memory. Create() should be called before using the object.
		ThreadMan();

		/// Makes the TimerMan object ready for use.
		void Initialize(){};

		/// Destructor method used to clean up a ThreadMan object before deletion
		/// from system memory.
		virtual ~ThreadMan();

		/// Makes the ThreadMan object ready for use.
		/// @return An error return value signaling sucess or any particular failure.
		/// Anything below 0 is an error signal.
		virtual int Create();

		/// Resets the entire ThreadMan, including its inherited members, to
		/// their default settings or values.
		virtual void Reset() { Clear(); }

		/// Destroys and resets (through Clear()) the ThreadMan object.
		void Destroy();

		BS::thread_pool& GetPriorityThreadPool() { return m_PriorityThreadPool; }

		BS::thread_pool& GetBackgroundThreadPool() { return m_BackgroundThreadPool; }

		/// Protected member variable and method declarations
	protected:
		/// Private member variable and method declarations
	private:
		/// Clears all the member variables of this ThreadMan, effectively
		/// resetting the members of this abstraction level only.
		void Clear();

		// Disallow the use of some implicit methods.
		ThreadMan(const ThreadMan& reference);
		ThreadMan& operator=(const ThreadMan& rhs);

		// For tasks that we want to be performed ASAP, i.e needs to be complete this frame at some point
		BS::thread_pool m_PriorityThreadPool;

		// For background tasks that we can just let happen whenever over multiple frames
		BS::thread_pool m_BackgroundThreadPool;
	};

} // namespace RTE
