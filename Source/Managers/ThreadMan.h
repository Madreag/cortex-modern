#pragma once

/// Header file for the ThreadMan class.
/// Author(s):
/// Inclusions of header files
#include "Singleton.h"
#define g_ThreadMan ThreadMan::Instance()

#include "BS_thread_pool.hpp"
#include "FloatingPointEnvironment.h"
#include "CheckpointFailure.h"
#include "SaverThreadMark.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace RTE {

	/// The threads that write saves run below the simulation's threads, and the work they hand out goes to a pool of their
	/// own that runs below them too, so a save never queues ahead of a tick's tasks.
	class SaverThreads {
	public:
		/// Lowers the calling thread below the simulation's threads for the rest of its life and marks it the saver's.
		static void Enter();

		/// Marks the calling thread the saver's without lowering it, for a thread the simulation may wait on.
		static void Mark() noexcept { SaverThreadMark::Set(); }

		/// Whether the calling thread is one of the saver's.
		static bool Current() noexcept { return SaverThreadMark::current; }

		/// Whether a game loop is handing the savers its idle time.
		static bool Gated() noexcept { return s_Gating.load(std::memory_order_relaxed) > 0; }

		/// The pool whose threads take the saver's parallel work.
		static BS::thread_pool& Pool();

		/// The saver's pool when a saver thread asks, else the pool asked for.
		static BS::thread_pool& PoolFor(BS::thread_pool& pool) { return SaverThreadMark::current ? Pool() : pool; }

		/// While the game runs, the saver's threads work only in its idle time: the game loop opens a slice when a tick's work
		/// is done and closes it when the next tick starts, and a saver thread calls this where it holds nothing the game needs
		/// and waits here while no slice is open. A game that is never idle gets a save that takes longer, never a slower tick.
		static void YieldToGame() {
			if (!SaverThreadMark::current || t_Ungated || !s_Gating.load(std::memory_order_relaxed) || s_Lent.load(std::memory_order_relaxed) > 0) return;
			CheckSlice();
		}

		/// Saver work on the calling thread that goes on beside the game instead of waiting for its idle time: copying a heap
		/// the ticks after a boundary write all over costs the game less than its own write faults on it would.
		class UngatedScope {
		public:
			UngatedScope() noexcept : m_Was(t_Ungated) { t_Ungated = true; }
			~UngatedScope() { t_Ungated = m_Was; }
			UngatedScope(const UngatedScope&) = delete;
			UngatedScope& operator=(const UngatedScope&) = delete;
		private:
			bool m_Was;
		};

		/// The game loop's side: the saver may work until the given steady-clock microsecond.
		static void OpenSlice(int64_t untilUs);

		/// The game loop's side: a tick starts, and the saver stops at its next yield.
		static void CloseSlice() noexcept;

		/// While this machine's own save is due and its writer is still busy, the saver works beside the game at the game's
		/// priority instead of in its idle time, so the due save is taken as soon as the writer can write it: the machine that
		/// is late pays for its own save. Otherwise a saver stays below the game, in a match as in single player.
		static void SetOverdue(bool overdue);

		/// Self-test: a saver thread given a slice keeps its lowest priority, and only an overdue save raises it to the game's.
		/// Empty when it does, else what was seen.
		static std::string PrioritySelfTestMismatch();

		/// The game loop's side when it has left the saver no idle time for a while: the tick starting now lends the saver its
		/// first microseconds, game work in flight or not, so a save in a world slower than its clock still completes.
		static void LendTickStart(int64_t microseconds);

		/// Whether a saver thread is parked waiting for a slice: a save is in progress and wants time.
		static bool Waiting() noexcept { return s_Parked.load(std::memory_order_relaxed) > 0; }

		/// The steady clock in microseconds, as the slices are measured.
		static int64_t NowUs() noexcept;

		/// The ticks the game loop has started, for saver work keyed on the ticks right after a boundary.
		static uint64_t TickStarts() noexcept { return s_TickStarts.load(std::memory_order_relaxed); }

		/// Game work that runs across ticks (a path solve, a horizon job) counts from its submission to its end, and savers
		/// stay parked while any is in flight: the time between ticks is not idle while it runs, and a saver beside it on the
		/// same core makes the next tick wait for it.
		static void GameWorkBegins() noexcept { s_GameWork.fetch_add(1, std::memory_order_acq_rel); }
		static void GameWorkEnds() noexcept;

		/// Ends one piece of game work when the task that does it ends, however it ends.
		class GameWorkScope {
		public:
			GameWorkScope() = default;
			~GameWorkScope() { GameWorkEnds(); }
			GameWorkScope(const GameWorkScope&) = delete;
			GameWorkScope& operator=(const GameWorkScope&) = delete;
		};

		/// Opens the gate while the game itself waits on the saver's work.
		class Lend {
		public:
			Lend();
			~Lend();
			Lend(const Lend&) = delete;
			Lend& operator=(const Lend&) = delete;
		};

		/// The game waits on the saver's work as on a due save its writer still holds: the gate opens and the saver runs at the
		/// game's priority until the wait ends.
		class Await {
		public:
			Await() : m_WasOverdue(s_Overdue.load(std::memory_order_acquire)) { SetOverdue(true); }
			~Await() { SetOverdue(m_WasOverdue); }
			Await(const Await&) = delete;
			Await& operator=(const Await&) = delete;
		private:
			Lend m_Lend;
			bool m_WasOverdue;
		};

		/// The game loop drives the slices while this is held; outside it (menus, loading) the saver runs freely.
		class Gating {
		public:
			Gating();
			~Gating();
			Gating(const Gating&) = delete;
			Gating& operator=(const Gating&) = delete;
		};

	private:
		static void CheckSlice();
		static void WaitForSlice();

		static inline thread_local bool s_Lowered = false;
		static inline thread_local bool t_Ungated = false;
		static inline std::atomic<int> s_Gating{0};
		static inline std::atomic<int> s_Lent{0};
		static inline std::atomic<int64_t> s_SliceEndUs{0};
		static inline std::atomic<int64_t> s_ClosedAtUs{0};
		static inline std::atomic<uint64_t> s_TickStarts{0};
		static inline std::atomic<int> s_GameWork{0};
		static inline std::atomic<bool> s_SliceLent{false};
		static inline std::atomic<int> s_Parked{0};
		static inline std::atomic<bool> s_Overdue{false};
	};

	/// Runs work(index) for every index below a count on a pool, the items taken in index order by whichever thread comes
	/// free first. The owner takes items too when it finishes, so it never waits behind other work queued on the pool for
	/// an item nobody has started; it waits only for the items already running.
	class ParallelWork {
	public:
		/// @param pool The pool whose threads help; the saver's own pool when a saver thread asks. @param count How many
		/// items. @param work The item's work; it outlives no Finish, so it may hold references to the owner's locals.
		/// @param helpers At most this many pool threads help.
		ParallelWork(BS::thread_pool& pool, size_t count, std::function<void(size_t)> work, size_t helpers = DefaultHelpers()) :
		    m_Shared(std::make_shared<Shared>()) {
			m_Shared->count = count;
			m_Shared->work = std::move(work);
			BS::thread_pool& helping = SaverThreads::PoolFor(pool);
			const size_t threads = std::min<size_t>({count > 0 ? count - 1 : 0, helpers, static_cast<size_t>(helping.get_thread_count())});
			try {
				for (size_t helper = 0; helper < threads; ++helper) {
					CheckpointFailure::Check(CheckpointFailure::Point::ParallelSubmission);
					helping.push_task([shared = m_Shared] { Run(*shared); });
				}
			} catch (...) {
				// Finish submitted readers before the constructor releases the caller's values.
				Finish(false);
				throw;
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
				SaverThreads::YieldToGame();
				const FloatingPointEnvironment::Scope scope("parallel callback");
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

	/// While a lockstep round runs, the game's process goes ahead of a desktop's ordinary programs. Windows gives the processor
	/// only to the highest priority ready work, so a process started below normal, or a game behind a busy foreground program,
	/// gets no time at all until that program goes idle, and its round feels a frozen machine.
	class RoundScheduling {
	public:
		/// Called as a round's coordinator arrives (true) and leaves (false); the priority the process had comes back when it leaves.
		/// @return The name of the priority the process was raised from, or nullptr when nothing changed.
		static const char* SetRoundRunning(bool running);

		/// The process's scheduling priority by name ("shared" where the scheduler shares the processor by weight).
		static const char* PriorityName();
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

		/// Keeps nested native checkpoint helpers separate from the Lua capture pool.
		BS::thread_pool& GetCheckpointThreadPool() {
			std::call_once(m_CheckpointPoolOnce, [this] {
				m_CheckpointThreadPool = std::make_unique<BS::thread_pool>(std::max(1u, std::thread::hardware_concurrency()));
			});
			return *m_CheckpointThreadPool;
		}

		/// The saver's own pool: its threads run below the simulation's.
		BS::thread_pool& GetSaverThreadPool() {
			std::call_once(m_SaverPoolOnce, [this] {
				m_SaverThreadPool = std::make_unique<BS::thread_pool>(std::max(1u, std::thread::hardware_concurrency()), [] {
					FloatingPointEnvironment::Initialize();
					SaverThreads::Enter();
				}, [] { FloatingPointEnvironment::Assert("saver task"); });
			});
			return *m_SaverThreadPool;
		}

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
		std::unique_ptr<BS::thread_pool> m_CheckpointThreadPool;
		std::once_flag m_CheckpointPoolOnce;
		std::unique_ptr<BS::thread_pool> m_SaverThreadPool;
		std::once_flag m_SaverPoolOnce;
	};

} // namespace RTE
