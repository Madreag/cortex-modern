#pragma once

#include "FloatingPointEnvironment.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_vmevent.h"
}

#include "CaptureSentinel.h"
#include "PageWriteFence.h"
#include "CheckpointFailure.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/mman.h>
#endif

namespace RTE::CheckpointLua {
	/// Keeps worker API allocations inside Lua's memory-error boundary.
	template<class Work> void ProtectedCall(lua_State* state, const Work& work) {
		struct Call {
			const Work& work;
			std::exception_ptr failure;
			static int Run(lua_State* state) {
				auto& call = *static_cast<Call*>(lua_touserdata(state, 1));
				try {
					lua_settop(state, 0);
					call.work();
				} catch (const std::exception&) { call.failure = std::current_exception(); }
				return 0;
			}
		} call{work};
		const int status = lua_cpcall(state, Call::Run, &call);
		if (call.failure) std::rethrow_exception(call.failure);
		if (status == LUA_ERRMEM) throw std::bad_alloc();
		if (status) {
			const char* reason = lua_tostring(state, -1);
			throw std::runtime_error(reason ? reason : "protected checkpoint Lua call failed");
		}
	}


	struct AllocationStats {
		size_t bytes = 0;
		size_t blocks = 0;
	};

	class HeapOwner;

	/// What one freeze's page copy cost, as it landed: its pages and bytes, the copy's own time, the copy buffer it had
	/// to map fresh (every page of that is a first-touch fault), and how long after the freeze began the copy landed.
	struct CopyReceipt {
		uint64_t generation = 0; // 0: no copy landed since the last one taken.
		size_t pages = 0;
		int64_t copyUs = 0;
		size_t freshBytes = 0;
		int64_t landedUs = 0;
	};

	struct CopyFaultStats {
		std::atomic<size_t> count{0};
		std::atomic<int64_t> us{0};
	};

	struct LibraryHandlers {
		lua_CFunction panic = nullptr, finalizerError = nullptr;
		uint8_t eventMask = 0;
	};

	// The VM's memory at one freeze: a copy of every committed page. Addresses identify the source VM; only the copy is read.
	class Snapshot {
	public:
		static constexpr size_t c_PageBytes = 4096;
		struct Page { std::byte bytes[c_PageBytes]; };

		Snapshot() = default;
		lua_State* State() const { return m_Data ? m_Data->state : nullptr; }
		LibraryHandlers Handlers() const { return m_Data ? m_Data->handlers : LibraryHandlers{}; }
		uint64_t StateSerial() const { return m_Data ? m_Data->serial : 0; }
		size_t ByteCount() const { return m_Data ? m_Data->committed : 0; }
		size_t BlockCount() const { return m_Data ? m_Data->copied.load(std::memory_order_relaxed) : 0; }
		int64_t FreezeUs() const { return m_Data ? m_Data->freezeUs : 0; }
		int64_t CopyUs() const { return m_Data ? m_Data->copyUs.load(std::memory_order_relaxed) : 0; }
		size_t FaultCount() const { return m_Data && m_Data->faults ? m_Data->faults->count.load(std::memory_order_relaxed) : 0; }
		size_t PreviousFaults() const { return m_Data ? m_Data->previousFaults : 0; }
		int64_t PreviousFaultUs() const { return m_Data ? m_Data->previousFaultUs : 0; }

		template<class T> T Read(const T* address) const {
			static_assert(std::is_trivially_copyable_v<T>);
			const auto bytes = ReadBytes(address, sizeof(T));
			T value;
			std::memcpy(&value, bytes.data(), sizeof(value));
			return value;
		}

		// The view stays valid while a copy of this snapshot remains alive.
		std::span<const std::byte> ReadBytes(const void* address, size_t size) const {
			if (!m_Data) throw std::runtime_error("a Lua heap snapshot is empty");
			if (size == 0) return {};
			m_Data->WaitCopied();
			const uintptr_t source = reinterpret_cast<uintptr_t>(address);
			if (source < m_Data->base || size > m_Data->committed || source - m_Data->base > m_Data->committed - size)
				throw std::runtime_error("a Lua heap read lies outside the recorded heap");
			const size_t offset = source - m_Data->base;
			// The copied pages occupy one contiguous mapping.
			return {reinterpret_cast<const std::byte*>(m_Data->pages) + offset, size};
		}

		std::string ReadString(const char* address, size_t size) const {
			const auto bytes = ReadBytes(address, size);
			return size ? std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()) : std::string();
		}
		TValue TableValue(const GCtab* source, int key) const {
			const auto table = Read(source);
			if (key >= 0 && static_cast<MSize>(key) < table.asize) return Read(mref(table.array, TValue) + key);
			const Node* nodes = mref(table.node, Node);
			for (MSize index = 0; index <= table.hmask; ++index) {
				const auto node = Read(nodes + index);
				if (tvisnumber(&node.key) && numberVnum(&node.key) == key) return node.val;
			}
			TValue nil; setnilV(&nil); return nil;
		}
		TValue TableValue(const GCtab* source, std::string_view key) const {
			const auto table = Read(source);
			const Node* nodes = mref(table.node, Node);
			for (MSize index = 0; index <= table.hmask; ++index) {
				const auto node = Read(nodes + index);
				if (tvisnil(&node.val) || !tvisstr(&node.key)) continue;
				const auto* string = strV(&node.key);
				if (Read(&string->len) != key.size()) continue;
				const auto bytes = ReadBytes(string + 1, key.size());
				if (key.empty() || std::memcmp(bytes.data(), key.data(), key.size()) == 0) return node.val;
			}
			TValue nil; setnilV(&nil); return nil;
		}
		TValue RegistryValue(int reference) const {
			const auto state = Read(State());
			const auto globals = Read(mref(state.glref, global_State));
			if (!tvistab(&globals.registrytv)) throw std::runtime_error("a frozen Lua registry is not a table");
			return TableValue(tabV(&globals.registrytv), reference);
		}
		TValue GlobalValue(std::string_view name) const { return TableValue(tabref(Read(State()).env), name); }
		TValue FunctionUpvalue(const GCfunc* source, size_t index) const {
			if (index >= Read(&source->c.nupvalues)) throw std::out_of_range("frozen Lua upvalue index");
			if (Read(&source->c.ffid) != FF_LUA) return Read(&source->c.upvalue[index]);
			const auto cell = Read(gco2uv(gcref(Read(&source->l.uvptr[index]))));
			return cell.closed ? cell.tv : Read(mref(cell.v, TValue));
		}

	private:
		struct Data {
			lua_State* state = nullptr;
			LibraryHandlers handlers;
			uint64_t serial = 0;
			uintptr_t base = 0;
			size_t committed = 0;
			std::atomic<size_t> copied{0};
			int64_t freezeUs = 0;
			std::atomic<int64_t> copyUs{0};
			std::shared_ptr<CopyFaultStats> faults;
			size_t previousFaults = 0;
			int64_t previousFaultUs = 0;
			std::shared_ptr<const void> buffer; // Keeps the copy mapped.
			const Page* pages = nullptr; // One per committed page, in order.
			std::shared_future<void> ready; // Set only when a freeze was given somewhere to run its copy.
			mutable std::atomic<bool> copiedFlag{false};
			void WaitCopied() const {
				if (copiedFlag.load(std::memory_order_acquire)) return;
				// A copy that failed left pages unread, so its reader fails with it instead of reading zeros.
				if (ready.valid()) ready.get();
				copiedFlag.store(true, std::memory_order_release);
			}
		};
		std::shared_ptr<const Data> m_Data;
		explicit Snapshot(std::shared_ptr<const Data> data) : m_Data(std::move(data)) {}

		friend class HeapOwner;
	};

	// Threads that copy frozen heaps while the thread that froze them runs on. A task never enters a VM.
	class CopyPool {
	public:
		static std::future<void> Submit(std::function<void()> work) {
			return Get().Push(std::move(work));
		}
		/// Detects partial startup cleanup and a successful retry.
		static bool RunFailureSelfTest() {
			std::atomic<size_t> exited{0};
			bool refused = false;
			{
				CheckpointFailure::Scope failure(CheckpointFailure::Point::CopyStartup, 1);
				try { CopyPool failed(2, &exited); }
				catch (const std::bad_alloc&) { refused = failure.Triggered(); }
			}
			const bool joined = exited.load() == 1;
			CopyPool retry(2, &exited);
			std::atomic<bool> copied{false};
			retry.Push([&copied] { copied.store(true); }).get();
			return refused && joined && copied.load();
		}
		// The boundary prepares native values before queued page copies contend for memory.
		class PauseScope {
		public:
			explicit PauseScope(bool enabled) : m_Enabled(enabled) {
				if (!m_Enabled) return;
				CopyPool& pool = Get();
				std::lock_guard lock(pool.m_Mutex);
				++pool.m_Paused;
			}
			~PauseScope() {
				if (!m_Enabled) return;
				CopyPool& pool = Get();
				{
					std::lock_guard lock(pool.m_Mutex);
					--pool.m_Paused;
				}
				pool.m_Ready.notify_all();
			}
			PauseScope(const PauseScope&) = delete;
			PauseScope& operator=(const PauseScope&) = delete;
		private:
			bool m_Enabled;
		};

	private:
		static CopyPool& Get() {
			static CopyPool* pool = new CopyPool(); // Copies still finish during static teardown.
			return *pool;
		}
		std::mutex m_Mutex;
		std::condition_variable m_Ready;
		std::deque<std::packaged_task<void()>> m_Tasks;
		unsigned m_Paused = 0;
		bool m_Stopping = false;
		std::array<std::thread, 4> m_Threads;

		explicit CopyPool(unsigned threads = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u), std::atomic<size_t>* exited = nullptr) {
			try {
				for (unsigned index = 0; index < threads; ++index) {
					CheckpointFailure::Check(CheckpointFailure::Point::CopyStartup);
					m_Threads[index] = FloatingPointEnvironment::StartThread([this, exited] {
						struct Exit { std::atomic<size_t>* count; ~Exit() { if (count) count->fetch_add(1); } } exit{exited};
						while (true) {
							std::packaged_task<void()> task;
							{
								std::unique_lock lock(m_Mutex);
								m_Ready.wait(lock, [this] { return m_Stopping || (!m_Paused && !m_Tasks.empty()); });
								if (m_Stopping && m_Tasks.empty()) return;
								task = std::move(m_Tasks.front());
								m_Tasks.pop_front();
							}
							CaptureSentinel::WorkerScope worker("heap-copy");
							const FloatingPointEnvironment::Scope scope("Lua copy task");
							task();
						}
					});
				}
			} catch (...) {
				Stop();
				throw;
			}
		}
		~CopyPool() { Stop(); }
		void Stop() {
			{
				std::lock_guard lock(m_Mutex);
				m_Stopping = true;
			}
			m_Ready.notify_all();
			for (auto& thread: m_Threads) if (thread.joinable()) thread.join();
		}
		std::future<void> Push(std::function<void()> work) {
			std::packaged_task<void()> task(std::move(work));
			std::future<void> done = task.get_future();
			{
				std::lock_guard lock(m_Mutex);
				m_Tasks.push_back(std::move(task));
			}
			m_Ready.notify_one();
			return done;
		}
	};

	// Owns the VM and the reservation every block comes from. A freeze copies every committed page into a buffer
	// the next freeze reuses once the snapshot that held it lets it go.
	struct HeapFreezeCosts { int64_t setupUs = 0; PageWriteFence::CopyWatchCosts watch; };

	class HeapOwner {
		struct CowCopy;
		struct CowCoordinator;
	public:
		class Prepared;
		static std::unique_ptr<HeapOwner> Create() {
			auto owner = std::unique_ptr<HeapOwner>(new HeapOwner());
			owner->Initialize();
			{
				std::lock_guard lock(RegistryMutex());
				LiveHeaps().emplace(owner.get(), owner->m_Identity);
			}
			return owner;
		}

		~HeapOwner() {
			std::lock_guard preparations(m_PrepareMutex);
			FinishPrepared();
			WaitCopy(true);
			PageWriteFence::UnwatchCopies(this);
			m_CowCopy.reset();
			m_CowCoordinator.reset();
			// From here a buffer released anywhere, by a snapshot that outlives this heap too, unmaps itself.
			{
				std::lock_guard lock(RegistryMutex());
				LiveHeaps().erase(this);
			}
			{
				std::lock_guard lock(m_SlabMutex);
				for (const auto& slab: m_IdleSlabs) {
					if (!slab) continue;
					CopyBytes(true).fetch_sub(slab->capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
					slab->owner = nullptr;
				}
				for (auto& slab: m_IdleSlabs) slab.reset();
			}
			// The wrapper keeps lua_close from destroying the bootstrap's shared arena.
			if (m_State) lua_close(std::exchange(m_State, nullptr));
			m_Bootstrap.reset();
			Release();
		}
		HeapOwner(const HeapOwner&) = delete;
		HeapOwner& operator=(const HeapOwner&) = delete;
		HeapOwner(HeapOwner&&) = delete;
		HeapOwner& operator=(HeapOwner&&) = delete;

		lua_State* State() const { return m_State; }

		AllocationStats Stats() const { return {m_Bytes, m_Blocks}; }

		/// Every copy buffer any heap has mapped, idle ones included, and the idle part of it.
		static size_t MappedCopyBytes() { return CopyBytes(false).load(std::memory_order_relaxed); }
		static size_t IdleCopyBytes() { return CopyBytes(true).load(std::memory_order_relaxed); }
		/// The copy buffers of this heap that a snapshot still holds.
		size_t LiveSlabs() const { return m_LiveSlabs.load(std::memory_order_relaxed); }
		/// The buffer a snapshot holds and the one the next freeze fills while it still holds it.
		static constexpr size_t c_LiveSlabs = 2;
		/// Buffers handed back to a heap that no longer exists; any is a use after free.
		static size_t CallsIntoDestroyedHeaps() { return DeadHeapCalls().load(std::memory_order_relaxed); }
		static bool RunBatchOpenSelfTest();
		static bool RunPreparedSelfTest();
		static bool RunReusedOwnerSelfTest();
		/// Prepares storage and watches on the saver; Arm reads no Lua objects and allocates nothing.
		std::shared_ptr<Prepared> PrepareFreeze();

		using Submit = std::function<std::future<void>(std::function<void()>)>;

		// The default gate holds VM entry until the submitted copy lands. A checkpoint's page fence instead
		// saves a page before its next write; without a Submit either copy runs here.
		/// @param armLater When given, the page fences are left to the returned work instead of being set here; it must finish
		/// before the VM runs again and before the copy task can start.
		Snapshot Freeze(const Submit& submit, bool copyOnWrite = false, bool batchCopy = false, HeapFreezeCosts* costs = nullptr, std::function<void()>* armLater = nullptr) {
			std::lock_guard preparations(m_PrepareMutex);
			FinishPrepared();
			const auto started = std::chrono::steady_clock::now();
			if (!m_State) throw std::runtime_error("a Lua heap capture has no state");
			void* allocatorData = nullptr;
			if (lua_getallocf(m_State, &allocatorData) != &Allocate || allocatorData != this)
				throw std::runtime_error("the Lua heap allocator changed after tracking began");
			const bool keepCow = copyOnWrite && m_CowCoordinator;
			if (!keepCow) WaitCopy(true);
			auto data = std::make_shared<Snapshot::Data>();
			data->state = m_State;
			data->handlers = m_LibraryHandlers;
			data->serial = G(m_State)->objserial;
			data->base = m_Base;
			data->committed = m_Committed;
			if (m_CowCopy) {
				data->previousFaults = m_CowCopy->faults->count.load(std::memory_order_relaxed);
				data->previousFaultUs = m_CowCopy->faults->us.load(std::memory_order_relaxed);
				if (!keepCow) {
					PageWriteFence::UnwatchCopies(this);
					m_CowCopy.reset();
					m_CowCoordinator.reset();
				}
			}
			m_CowGateFree.store(keepCow, std::memory_order_release);
			m_FreshBytes = 0;
			auto done = submit ? std::make_shared<std::promise<void>>() : nullptr;
			if (done) {
				data->ready = done->get_future().share();
				std::lock_guard lock(m_CopyMutex);
				std::erase_if(m_PreviousCopies, [](const auto& future) { return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
				m_PreviousCopies.reserve(m_PreviousCopies.size() + 1);
			}
			struct Rollback {
				HeapOwner& owner;
				std::shared_ptr<CowCopy> previous;
				bool keepCow, committed = false;
				~Rollback() {
					if (committed) return;
					if (owner.m_CowCopy != previous && owner.m_CowCoordinator) owner.m_CowCoordinator->Cancel(owner.m_CowCopy);
					owner.m_CowCopy = std::move(previous);
					owner.m_CowGateFree.store(keepCow, std::memory_order_release);
				}
			} rollback{*this, m_CowCopy, keepCow};
			if (copyOnWrite && data->committed) {
				const size_t pageBytes = PageWriteFence::SystemPageBytes();
				if (!pageBytes || m_Base % pageBytes || data->committed % pageBytes)
					throw std::runtime_error("the Lua heap is not aligned for a page copy fence");
				auto slab = TakeSlab(data->committed / Snapshot::c_PageBytes, batchCopy);
				data->pages = slab->pages;
				data->buffer = std::move(slab);
				data->faults = std::make_shared<CopyFaultStats>();
				auto cow = std::make_shared<CowCopy>();
				cow->base = m_Base;
				cow->pageBytes = pageBytes;
				cow->destination = const_cast<Snapshot::Page*>(data->pages);
				cow->saved.assign(data->committed / pageBytes, 0);
				cow->faults = data->faults;
				cow->buffer = data->buffer;
				if (!m_CowCoordinator) {
					m_CowCoordinator = std::make_shared<CowCoordinator>();
					m_CowCoordinator->base = m_Base;
					m_CowCoordinator->pageBytes = pageBytes;
				}
				if (costs) costs->setupUs = MicrosecondsSince(started);
				m_CowCopy = cow;
				m_CowCoordinator->Prepare(cow);
				if (armLater && submit) {
					*armLater = [this, cow, coordinator = m_CowCoordinator, committed = data->committed] {
						CowCoordinator::Prepared prepared{coordinator.get(), cow};
						if (!PageWriteFence::WatchCopies(this, {reinterpret_cast<uint8_t*>(m_Base), committed}, CowCoordinator::OnWrite, coordinator.get(), CowCoordinator::Arm, &prepared)) {
							coordinator->Cancel(cow);
							throw std::runtime_error("could not fence the Lua heap page copy");
						}
						m_CowGateFree.store(true, std::memory_order_release);
					};
				} else {
					CowCoordinator::Prepared prepared{m_CowCoordinator.get(), cow};
					if (!PageWriteFence::WatchCopies(this, {reinterpret_cast<uint8_t*>(m_Base), data->committed}, CowCoordinator::OnWrite,
					                                m_CowCoordinator.get(), CowCoordinator::Arm, &prepared, costs ? &costs->watch : nullptr))
						throw std::runtime_error("could not fence the Lua heap page copy");
					m_CowGateFree.store(true, std::memory_order_release);
				}
			}
			const bool receipt = Receipts().load(std::memory_order_acquire);
			const size_t freshBytes = m_FreshBytes;
			auto copy = [this, data, started, receipt, freshBytes, batchCopy, cow = m_CowCopy, coordinator = m_CowCoordinator] {
				if (cow) {
					// A deferred fence must own this generation before any page is copied.
					while (cow->armed.load(std::memory_order_acquire) == 0) cow->armed.wait(0, std::memory_order_acquire);
					if (cow->armed.load(std::memory_order_acquire) != 1) throw std::runtime_error("the Lua heap copy fence was cancelled");
					const auto copying = std::chrono::steady_clock::now();
					// Bounded page runs avoid repeated protection calls while live faults still save individual pages.
					const size_t batchPages = batchCopy ? 64 : 1;
					for (size_t page = 0; page < cow->saved.size(); page += batchPages) {
						const bool opened = batchCopy ? coordinator->CopyPages(page, std::min(batchPages, cow->saved.size() - page))
						                             : coordinator->CopyPage(page, false);
						if (!opened)
							throw std::runtime_error("could not open copied Lua heap pages");
					}
					if (!coordinator->Complete(cow, batchCopy)) throw std::runtime_error("could not open a copied Lua heap");
					data->copied.store(data->committed / Snapshot::c_PageBytes, std::memory_order_relaxed);
					data->copyUs.store(MicrosecondsSince(copying), std::memory_order_relaxed);
				} else {
					CopyPages(*data);
				}
				if (!receipt) return;
				std::lock_guard lock(m_CopyMutex);
				m_LandedCopy = {++m_LandedGeneration, data->copied.load(std::memory_order_relaxed), data->copyUs.load(std::memory_order_relaxed), freshBytes, MicrosecondsSince(started)};
				LandedCopies().fetch_add(1, std::memory_order_release);
			};
			if (submit) {
				// The snapshot waits on a promise of its own: a task's future can keep the task, and with it this
				// snapshot, alive for as long as the snapshot holds that future.
				{
					std::lock_guard lock(m_CopyMutex);
					if (m_PendingCopy.valid() && m_PendingCopy.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
						m_PreviousCopies.push_back(m_PendingCopy);
					m_PendingCopy = data->ready;
					++m_CopyGeneration;
					m_CopyPending.store(true, std::memory_order_release);
				}
				try {
					CheckpointFailure::Check(CheckpointFailure::Point::LuaSubmission);
					submit([copy = std::move(copy), done, cow = m_CowCopy, coordinator = m_CowCoordinator] {
						try {
							copy();
							done->set_value();
						} catch (...) {
							if (cow && coordinator) coordinator->Cancel(cow);
							done->set_exception(std::current_exception());
						}
					});
				} catch (...) {
					done->set_exception(std::current_exception());
					throw;
				}
			} else {
				copy();
			}
			rollback.committed = true;
			data->freezeUs = MicrosecondsSince(started);
			return Snapshot(std::move(data));
		}

		// Page-fenced generations share one coordinator. Only destruction or a
		// switch back to the legacy entry gate needs their tasks to finish.
		void WaitCopy(bool force = false) {
			if (!force && m_CowGateFree.load(std::memory_order_acquire)) return;
			if (!m_CopyPending.load(std::memory_order_acquire)) return;
			std::unique_lock lock(m_CopyMutex);
			if (!m_PendingCopy.valid()) return;
			const uint64_t generation = m_CopyGeneration;
			const size_t count = m_PreviousCopies.size();
			for (size_t index = 0; index <= count; ++index) {
				const auto future = index < count ? m_PreviousCopies[index] : m_PendingCopy;
				lock.unlock();
				if (future.valid() && future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
					const auto waited = std::chrono::steady_clock::now();
					future.wait();
					const int64_t waitedUs = MicrosecondsSince(waited);
					GateWaitUs().fetch_add(waitedUs, std::memory_order_relaxed);
					ThreadGateWaitUs() += waitedUs;
				}
				lock.lock();
				if (generation != m_CopyGeneration) { lock.unlock(); WaitCopy(force); return; }
			}
			if (generation == m_CopyGeneration) {
				m_PendingCopy = {};
				m_PreviousCopies.clear();
				m_CopyPending.store(false, std::memory_order_release);
			}
		}
		bool CopyPending() const { return m_CopyPending.load(std::memory_order_acquire); }
		/// Microseconds any thread has spent waiting at a gate for a copy, summed over every heap.
		static int64_t GateWaitMicroseconds() { return GateWaitUs().load(std::memory_order_relaxed); }
		/// The calling thread's own share of it: the simulation's stall, apart from capture workers that called into a frozen state.
		static int64_t ThisThreadGateWaitMicroseconds() { return ThreadGateWaitUs(); }

		/// While one is open, the freezes it covers leave receipts of what their copies cost: a match's capture diagnostic,
		/// so a single-player save lands its copies with none. Opened by the thread that starts the capture, before its workers.
		class ReceiptScope {
		public:
			explicit ReceiptScope(bool open) : m_Was(Receipts().exchange(open, std::memory_order_acq_rel)) {}
			~ReceiptScope() { Receipts().store(m_Was, std::memory_order_release); }
			ReceiptScope(const ReceiptScope&) = delete;
			ReceiptScope& operator=(const ReceiptScope&) = delete;

		private:
			bool m_Was;
		};

		/// How many receipted copies have landed on any heap: a reader with nothing new to take reads this alone.
		static uint64_t LandedCopyCount() { return LandedCopies().load(std::memory_order_acquire); }
		/// The receipt of the copy that landed last, once; call it after the gate.
		CopyReceipt TakeCopyReceipt() {
			std::lock_guard lock(m_CopyMutex);
			return std::exchange(m_LandedCopy, {});
		}

	private:
		static constexpr size_t c_ReserveBytes = size_t(1) << 32; // Address space only; committed as the VM grows.
		static constexpr size_t c_CommitStep = size_t(1) << 20;
		static constexpr size_t c_LargeLimit = size_t(1) << 18; // Above this a block takes whole pages of its own.
		static constexpr size_t c_ClassCount = 64 + 56 + 62;
		// Heaps are reserved whole gigabytes apart and lay out alike, so each one starts its blocks on its own
		// cache sets and its own pages: an odd count of lines along, and an odd count of the largest page a
		// platform maps (16 KB) along, so the same block of every state takes neither one set nor one TLB entry.
		static constexpr size_t c_ColorStride = 33 * 64;
		static constexpr size_t c_PageStride = 13 * 16384;
		static constexpr size_t c_Colors = 64;

		static size_t NextColor() {
			static std::atomic<size_t> next{0};
			return next.fetch_add(1, std::memory_order_relaxed) % c_Colors;
		}

		HeapOwner() = default;
		static uint64_t NextIdentity() {
			static std::atomic<uint64_t> next{0};
			return next.fetch_add(1, std::memory_order_relaxed) + 1;
		}
		const uint64_t m_Identity = NextIdentity();
		std::unique_ptr<lua_State, decltype(&lua_close)> m_Bootstrap{nullptr, lua_close};
		lua_State* m_State = nullptr;
		LibraryHandlers m_LibraryHandlers;
		uintptr_t m_Reservation = 0;
		uintptr_t m_Base = 0;
		size_t m_Committed = 0;
		std::atomic<size_t> m_PublishedCommitted{0};
		size_t m_Used = 0;
		size_t m_Bytes = 0;
		size_t m_Blocks = 0;
		void* m_Free[c_ClassCount] = {};
		std::vector<std::pair<void*, size_t>> m_LargeFree; // Whole-page blocks given back, by their rounded size.

		// A copy buffer mapped straight from the system and kept for reuse: a reused one has its pages resident.
		struct Slab {
			Snapshot::Page* pages = nullptr;
			size_t capacity = 0;
			HeapOwner* owner = nullptr;
			uint64_t ownerIdentity = 0;
			~Slab();
		};
		static constexpr size_t c_IdleSlabs = 1; // With one capture in flight, the next freeze reuses the buffer the last one gave back.
		std::mutex m_SlabMutex;
		std::array<std::unique_ptr<Slab>, c_IdleSlabs> m_IdleSlabs;
		std::atomic<size_t> m_LiveSlabs{0};
		std::mutex m_CopyMutex;
		std::shared_future<void> m_PendingCopy;
		std::vector<std::shared_future<void>> m_PreviousCopies;
		uint64_t m_CopyGeneration = 0;
		std::atomic<bool> m_CopyPending{false};
		std::atomic<bool> m_CowGateFree{false};
		std::mutex m_PrepareMutex;
		std::vector<std::weak_ptr<Prepared>> m_Prepared;
		void FinishPrepared() noexcept;
		struct CowCopy {
			uintptr_t base = 0;
			size_t pageBytes = 0;
			Snapshot::Page* destination = nullptr;
			std::vector<uint8_t> saved;
			std::shared_ptr<CopyFaultStats> faults;
			std::shared_ptr<const void> buffer;
			std::atomic<int> armed{0};
			bool completed = false;
		};
		struct CowCoordinator {
			uintptr_t base = 0;
			size_t pageBytes = 0;
			std::vector<std::shared_ptr<CowCopy>> copies;
			std::atomic_flag lock = ATOMIC_FLAG_INIT;
			struct Locked {
				std::atomic_flag& flag;
				explicit Locked(std::atomic_flag& flag) : flag(flag) { while (flag.test_and_set(std::memory_order_acquire)) {} }
				~Locked() { flag.clear(std::memory_order_release); }
			};
			struct Prepared { CowCoordinator* coordinator; std::shared_ptr<CowCopy> copy; };
			void Prepare(const std::shared_ptr<CowCopy>& copy) {
				Locked guard(lock);
				std::erase_if(copies, [](const auto& value) { return value->completed; });
				copies.push_back(copy);
			}
			static bool Arm(void* context, uintptr_t address, size_t bytes) noexcept {
				auto& prepared = *static_cast<Prepared*>(context);
				auto& coordinator = *prepared.coordinator;
				Locked guard(coordinator.lock);
				if (prepared.copy->completed || prepared.copy->armed.load(std::memory_order_relaxed) != 0) return false;
				if (PageWriteFence::ProtectCopyPages(address, bytes)) {
					prepared.copy->armed.store(1, std::memory_order_release);
					prepared.copy->armed.notify_all();
					return true;
				}
				return false;
			}

			bool CopyPage(size_t page, bool fault) noexcept {
				const auto started = fault ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
				Locked guard(lock);
				const uintptr_t address = base + page * pageBytes;
				bool savedAny = false;
				for (const auto& copy: copies) {
					if (copy->armed.load(std::memory_order_relaxed) != 1 || copy->completed) continue;
					if (page >= copy->saved.size() || copy->saved[page]) continue;
					// An unsaved generation's page has never been opened for a
					// write. Materialize every such generation before opening it.
					std::memcpy(reinterpret_cast<std::byte*>(copy->destination) + page * pageBytes, reinterpret_cast<const void*>(address), pageBytes);
					copy->saved[page] = 1;
					savedAny = true;
					if (fault) {
						copy->faults->count.fetch_add(1, std::memory_order_relaxed);
						copy->faults->us.fetch_add(MicrosecondsSince(started), std::memory_order_relaxed);
					}
				}
				return (!fault && !savedAny) || PageWriteFence::OpenCopiedPage(address, pageBytes);
			}
			bool CopyPages(size_t first, size_t count) noexcept {
				Locked guard(lock);
				for (const auto& copy: copies) {
					if (copy->armed.load(std::memory_order_relaxed) != 1 || copy->completed) continue;
					const size_t last = std::min(first + count, copy->saved.size());
					for (size_t page = first; page < last;) {
						if (copy->saved[page]) { ++page; continue; }
						const size_t begin = page;
						while (page < last && !copy->saved[page]) ++page;
						std::memcpy(reinterpret_cast<std::byte*>(copy->destination) + begin * pageBytes,
						            reinterpret_cast<const void*>(base + begin * pageBytes), (page - begin) * pageBytes);
						std::fill(copy->saved.begin() + begin, copy->saved.begin() + page, 1);
					}
				}
				// Every active generation owns this batch before live writes resume on it.
				return PageWriteFence::OpenCopiedPage(base + first * pageBytes, count * pageBytes);
			}
			bool Complete(const std::shared_ptr<CowCopy>& copy, bool openHeap) {
				Locked guard(lock);
				copy->completed = true;
				copy->buffer.reset();
				if (!openHeap) return true;
				size_t pages = 0;
				for (const auto& generation: copies) {
					if (generation->armed.load(std::memory_order_relaxed) != 1) continue;
					if (!generation->completed) return true;
					pages = std::max(pages, generation->saved.size());
				}
				return PageWriteFence::OpenCopiedPage(base, pages * pageBytes);
			}
			void Cancel(const std::shared_ptr<CowCopy>& copy) {
				Locked guard(lock);
				std::erase(copies, copy);
				if (copy) {
					copy->armed.store(2, std::memory_order_release);
					copy->armed.notify_all();
					copy->completed = true; copy->buffer.reset();
				}
				if (std::all_of(copies.begin(), copies.end(), [](const auto& generation) { return generation->armed.load(std::memory_order_relaxed) != 1 || generation->completed; }) && copy)
					PageWriteFence::OpenCopiedPage(base, copy->saved.size() * pageBytes);
			}
			static bool OnWrite(void* context, uintptr_t address) noexcept {
				auto& coordinator = *static_cast<CowCoordinator*>(context);
				return coordinator.CopyPage((address - coordinator.base) / coordinator.pageBytes, true);
			}
		};
		std::shared_ptr<CowCopy> m_CowCopy;
		std::shared_ptr<CowCoordinator> m_CowCoordinator;
		size_t m_FreshBytes = 0; // The copy task's own: what its buffers mapped fresh.
		uint64_t m_LandedGeneration = 0;
		CopyReceipt m_LandedCopy; // Under m_CopyMutex.
		static std::atomic<int64_t>& GateWaitUs() {
			static std::atomic<int64_t> waited{0};
			return waited;
		}
		static std::atomic<uint64_t>& LandedCopies() {
			static std::atomic<uint64_t> landed{0};
			return landed;
		}
		static std::atomic<bool>& Receipts() {
			static std::atomic<bool> open{false};
			return open;
		}
		static int64_t& ThreadGateWaitUs() {
			thread_local int64_t waited = 0;
			return waited;
		}
		static std::atomic<size_t>& CopyBytes(bool idle) {
			static std::atomic<size_t> mapped{0}, idleBytes{0};
			return idle ? idleBytes : mapped;
		}
		// Every heap that still exists; a buffer goes back only to one of these. Never destroyed, so a
		// snapshot released during static teardown still finds it.
		static std::mutex& RegistryMutex() {
			static std::mutex* mutex = new std::mutex();
			return *mutex;
		}
		static std::unordered_map<const HeapOwner*, uint64_t>& LiveHeaps() {
			static auto* heaps = new std::unordered_map<const HeapOwner*, uint64_t>();
			return *heaps;
		}
		static std::atomic<size_t>& DeadHeapCalls() {
			static std::atomic<size_t> calls{0};
			return calls;
		}
		std::shared_ptr<Slab> TakeSlab(size_t pages, bool bounded = false) {
			CheckpointFailure::Check(CheckpointFailure::Point::LuaPages);
			if (pages > std::numeric_limits<size_t>::max() / Snapshot::c_PageBytes) throw std::bad_alloc();
			const size_t held = m_LiveSlabs.fetch_add(1, std::memory_order_relaxed);
			struct Reservation {
				std::atomic<size_t>& count;
				bool transferred = false;
				~Reservation() { if (!transferred) count.fetch_sub(1, std::memory_order_relaxed); }
			} reservation{m_LiveSlabs};
			// The writing and waiting captures leave one slot for the boundary being frozen.
			if (bounded && held >= c_LiveSlabs + 1) throw std::bad_alloc();
			std::unique_ptr<Slab> slab;
			{
				std::lock_guard lock(m_SlabMutex);
				const auto fit = std::find_if(m_IdleSlabs.begin(), m_IdleSlabs.end(), [pages](const auto& candidate) { return candidate && candidate->capacity >= pages; });
				if (fit != m_IdleSlabs.end()) {
					slab = std::move(*fit);
					CopyBytes(true).fetch_sub(slab->capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
				}
				if (!slab) for (auto& idle: m_IdleSlabs) if (idle) {
					CopyBytes(true).fetch_sub(idle->capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
					idle.reset();
				}
			}
			if (!slab) {
				slab = std::make_unique<Slab>();
				slab->owner = this;
				slab->ownerIdentity = m_Identity;
				slab->capacity = pages;
				slab->pages = static_cast<Snapshot::Page*>(MapPages(slab->capacity * Snapshot::c_PageBytes));
				if (!slab->pages) throw std::bad_alloc();
				m_FreshBytes += slab->capacity * Snapshot::c_PageBytes;
				CopyBytes(false).fetch_add(slab->capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
			}
			reservation.transferred = true;
			return std::shared_ptr<Slab>(slab.release(), [](Slab* returned) { ReturnSlab(returned->owner, returned); });
		}
		// A buffer goes back to its heap only while that heap exists; one released after it is unmapped instead.
		static void ReturnSlab(HeapOwner* owner, Slab* slab) {
			{
				std::lock_guard lock(RegistryMutex());
				const auto found = LiveHeaps().find(owner);
				if (found != LiveHeaps().end() && found->second == slab->ownerIdentity) {
					owner->GiveSlab(slab);
					return;
				}
			}
			slab->owner = nullptr;
			delete slab;
		}
		// The caller holds RegistryMutex.
		void GiveSlab(Slab* slab) {
			const auto found = LiveHeaps().find(this);
			if (found == LiveHeaps().end() || found->second != slab->ownerIdentity) {
				// Only the address is read: the heap behind it is gone, so the buffer is left mapped rather than touch it.
				DeadHeapCalls().fetch_add(1, std::memory_order_relaxed);
				return;
			}
			m_LiveSlabs.fetch_sub(1, std::memory_order_relaxed);
			std::lock_guard lock(m_SlabMutex);
			// The next freeze needs a buffer as big as the heap, so a full pool keeps its largest.
			const auto empty = std::find_if(m_IdleSlabs.begin(), m_IdleSlabs.end(), [](const auto& value) { return !value; });
			auto target = empty;
			if (target == m_IdleSlabs.end()) {
				const auto smallest = std::min_element(m_IdleSlabs.begin(), m_IdleSlabs.end(), [](const auto& a, const auto& b) { return a->capacity < b->capacity; });
				if ((*smallest)->capacity >= slab->capacity) {
					DropSlab(slab);
					return;
				}
				CopyBytes(true).fetch_sub((*smallest)->capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
				DropSlab(smallest->release());
				target = smallest;
			}
			target->reset(slab);
			CopyBytes(true).fetch_add(slab->capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
		}
		static void DropSlab(Slab* slab) {
			Unmap(slab->pages, slab->capacity * Snapshot::c_PageBytes);
			CopyBytes(false).fetch_sub(slab->capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
			slab->pages = nullptr;
			delete slab;
		}

		static int64_t MicrosecondsSince(std::chrono::steady_clock::time_point started) {
			return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
		}

		// Copies every committed page; nothing writes the heap meanwhile. A capture's collection marks every live object, so
		// nearly every page is written between two freezes: keeping the last copy to share the rest would hold a second
		// copy of the heap for a tenth of the copying, and the buffer it pins could never be reused.
		void CopyPages(Snapshot::Data& data) {
			const auto copyStarted = std::chrono::steady_clock::now();
			const size_t pageCount = data.committed / Snapshot::c_PageBytes;
			if (pageCount != 0) {
				std::shared_ptr<Slab> slab = TakeSlab(pageCount);
				std::memcpy(slab->pages, reinterpret_cast<const void*>(m_Base), pageCount * Snapshot::c_PageBytes);
				data.pages = slab->pages;
				data.buffer = std::move(slab);
			}
			data.copied.store(pageCount, std::memory_order_relaxed);
			data.copyUs.store(MicrosecondsSince(copyStarted), std::memory_order_relaxed);
		}

		// Classes: 16-byte steps to 1 KB, 128-byte steps to 8 KB, 4 KB steps to 256 KB.
		static size_t ClassOf(size_t size) {
			if (size <= 1024) return (size + 15) / 16 - 1;
			if (size <= 8192) return 64 + (size - 1024 + 127) / 128 - 1;
			return 64 + 56 + (size - 8192 + 4095) / 4096 - 1;
		}
		static size_t ClassBytes(size_t index) {
			if (index < 64) return (index + 1) * 16;
			if (index < 64 + 56) return 1024 + (index - 64 + 1) * 128;
			return 8192 + (index - 64 - 56 + 1) * 4096;
		}
		static size_t RoundPages(size_t size) { return (size + Snapshot::c_PageBytes - 1) / Snapshot::c_PageBytes * Snapshot::c_PageBytes; }

		void Initialize() {
#if !LJ_GC64
			throw std::runtime_error("frozen Lua heap capture requires the GC64 allocator interface");
#else
			const size_t color = NextColor();
			Reserve(color * c_PageStride);
			m_Used = color * c_ColorStride;
			m_Bootstrap.reset(luaL_newstate());
			if (!m_Bootstrap) throw std::runtime_error("could not create the Lua allocator bootstrap");
			const lua_CFunction panic = G(m_Bootstrap.get())->panic;
			m_LibraryHandlers.panic = panic;
#ifndef LUAJIT_DISABLE_VMEVENT
			lua_getfield(m_Bootstrap.get(), LUA_REGISTRYINDEX, LJ_VMEVENTS_REGKEY);
			if (!lua_istable(m_Bootstrap.get(), -1)) throw std::runtime_error("the Lua bootstrap supplied no finalizer event table");
			lua_rawgeti(m_Bootstrap.get(), -1, VMEVENT_HASH(LJ_VMEVENT_ERRFIN));
			const lua_CFunction finalizerError = lua_tocfunction(m_Bootstrap.get(), -1);
			if (!finalizerError || lua_getupvalue(m_Bootstrap.get(), -1, 1))
				throw std::runtime_error("the Lua bootstrap finalizer handler cannot be transferred");
			lua_pop(m_Bootstrap.get(), 2);
			m_LibraryHandlers.finalizerError = finalizerError;
			m_LibraryHandlers.eventMask = G(m_Bootstrap.get())->vmevmask;
#endif
			m_State = lua_newstate(&Allocate, this);
			if (!m_State) throw std::runtime_error("could not create the tracked Lua state");
			lua_atpanic(m_State, panic);
#ifndef LUAJIT_DISABLE_VMEVENT
			if (luaL_findtable(m_State, LUA_REGISTRYINDEX, LJ_VMEVENTS_REGKEY, LJ_VMEVENTS_HSIZE))
				throw std::runtime_error("could not create the tracked Lua finalizer event table");
			lua_pushcfunction(m_State, finalizerError);
			lua_rawseti(m_State, -2, VMEVENT_HASH(LJ_VMEVENT_ERRFIN));
			G(m_State)->vmevmask = G(m_Bootstrap.get())->vmevmask;
			lua_pop(m_State, 1);
#endif
#endif
		}

#ifdef _WIN32
		static void* MapPages(size_t bytes) noexcept { return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
		static void Unmap(void* pages, size_t) noexcept { VirtualFree(pages, 0, MEM_RELEASE); }
		void Reserve(size_t shift) {
			void* base = VirtualAlloc(nullptr, c_ReserveBytes + c_Colors * c_PageStride, MEM_RESERVE, PAGE_NOACCESS);
			if (!base) throw std::runtime_error("could not reserve the tracked Lua heap");
			m_Reservation = reinterpret_cast<uintptr_t>(base);
			m_Base = m_Reservation + shift;
		}
		bool Commit(size_t bytes) noexcept {
			return VirtualAlloc(reinterpret_cast<void*>(m_Base + m_Committed), bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
		}
		void Release() noexcept {
			if (m_Reservation) VirtualFree(reinterpret_cast<void*>(m_Reservation), 0, MEM_RELEASE);
			m_Reservation = m_Base = 0;
		}
#else
		static void* MapPages(size_t bytes) noexcept {
			void* pages = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			return pages == MAP_FAILED ? nullptr : pages;
		}
		static void Unmap(void* pages, size_t bytes) noexcept { munmap(pages, bytes); }
		void Reserve(size_t shift) {
			void* base = mmap(nullptr, c_ReserveBytes + c_Colors * c_PageStride, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
			if (base == MAP_FAILED) throw std::runtime_error("could not reserve the tracked Lua heap");
			m_Reservation = reinterpret_cast<uintptr_t>(base);
			m_Base = m_Reservation + shift;
		}
		bool Commit(size_t bytes) noexcept {
			return mprotect(reinterpret_cast<void*>(m_Base + m_Committed), bytes, PROT_READ | PROT_WRITE) == 0;
		}
		void Release() noexcept {
			if (m_Reservation) munmap(reinterpret_cast<void*>(m_Reservation), c_ReserveBytes + c_Colors * c_PageStride);
			m_Reservation = m_Base = 0;
		}
#endif

		void* Bump(size_t bytes) noexcept {
			if (bytes > c_ReserveBytes - m_Used) return nullptr;
			if (m_Used + bytes > m_Committed) {
				const size_t needed = m_Used + bytes - m_Committed;
				const size_t step = std::max(c_CommitStep, (needed + c_CommitStep - 1) / c_CommitStep * c_CommitStep);
				if (m_Committed + step > c_ReserveBytes || !Commit(step)) return nullptr;
				m_Committed += step;
				m_PublishedCommitted.store(m_Committed, std::memory_order_release);
			}
			void* result = reinterpret_cast<void*>(m_Base + m_Used);
			m_Used += bytes;
			return result;
		}

		void* Take(size_t size) noexcept {
			if (size > c_LargeLimit) {
				const size_t rounded = RoundPages(size);
				for (auto entry = m_LargeFree.begin(); entry != m_LargeFree.end(); ++entry) {
					if (entry->second != rounded) continue;
					void* block = entry->first;
					*entry = m_LargeFree.back();
					m_LargeFree.pop_back();
					return block;
				}
				// Whole pages start on a page, so a freed one can be given back as a whole.
				m_Used = RoundPages(m_Used);
				return Bump(rounded);
			}
			const size_t index = ClassOf(size);
			if (void* block = m_Free[index]) {
				m_Free[index] = *static_cast<void**>(block);
				return block;
			}
			return Bump(ClassBytes(index));
		}

		void Give(void* block, size_t size) noexcept {
			if (size > c_LargeLimit) {
				try { m_LargeFree.emplace_back(block, RoundPages(size)); } catch (...) {}
				return;
			}
			const size_t index = ClassOf(size);
			*static_cast<void**>(block) = m_Free[index];
			m_Free[index] = block;
		}

		// The VM calls this on the one thread running it and a freeze holds the VM's lock, so the ledger needs none.
		friend struct Slab;
		static void* Allocate(void* opaque, void* address, size_t previousSize, size_t size) noexcept {
			auto& owner = *static_cast<HeapOwner*>(opaque);
			if (size && CheckpointFailure::Fails(CheckpointFailure::Point::LuaAllocation)) return nullptr;
			// A VM that reached its heap past the state's lock still meets the gate before it writes a block.
			owner.WaitCopy();
			if (size == 0) {
				if (address) {
					owner.Give(address, previousSize);
					owner.m_Bytes -= previousSize;
					--owner.m_Blocks;
				}
				return nullptr;
			}
			if (!address) {
				void* block = owner.Take(size);
				if (block) { owner.m_Bytes += size; ++owner.m_Blocks; }
				return block;
			}
			const bool sameClass = previousSize <= c_LargeLimit && size <= c_LargeLimit && ClassOf(previousSize) == ClassOf(size);
			const bool sameLarge = previousSize > c_LargeLimit && size > c_LargeLimit && RoundPages(previousSize) == RoundPages(size);
			if (sameClass || sameLarge) {
				owner.m_Bytes = owner.m_Bytes - previousSize + size;
				return address;
			}
			void* block = owner.Take(size);
			if (!block) return nullptr;
			std::memcpy(block, address, std::min(previousSize, size));
			owner.Give(address, previousSize);
			owner.m_Bytes = owner.m_Bytes - previousSize + size;
			return block;
		}
	};

	class HeapOwner::Prepared {
	public:
		~Prepared() { Cancel(); }
		Prepared(const Prepared&) = delete;
		Prepared& operator=(const Prepared&) = delete;
		bool Current(const HeapOwner& owner) const noexcept {
			return m_Data->state == owner.m_State && m_Data->committed == owner.m_PublishedCommitted.load(std::memory_order_acquire);
		}
		bool Arm(HeapOwner& owner) noexcept {
			std::unique_lock preparations(owner.m_PrepareMutex, std::try_to_lock);
			if (!preparations || !Current(owner) || owner.m_CowCoordinator != m_Coordinator) return false;
			if (owner.m_CopyPending.load(std::memory_order_acquire) && !owner.m_CowGateFree.load(std::memory_order_acquire)) return false;
			std::unique_lock lock(m_Mutex, std::try_to_lock);
			if (!lock || m_Finished || m_Copy->armed.load(std::memory_order_relaxed) != 0) return false;
			const auto started = std::chrono::steady_clock::now();
			m_Data->serial = G(owner.m_State)->objserial;
			CowCoordinator::Prepared prepared{m_Coordinator.get(), m_Copy};
			if (!CowCoordinator::Arm(&prepared, m_Data->base, m_Data->committed)) return false;
			owner.m_CowGateFree.store(true, std::memory_order_release);
			m_Data->freezeUs = MicrosecondsSince(started);
			return true;
		}
		Snapshot Image() const { return Snapshot(m_Data); }
		void Drain() {
			std::lock_guard lock(m_Mutex);
			if (m_Finished) { m_Data->WaitCopied(); return; }
			if (m_Copy->armed.load(std::memory_order_acquire) != 1) throw std::logic_error("Lua heap copy requested before its prepared boundary");
			try {
				const auto started = std::chrono::steady_clock::now();
				constexpr size_t batchPages = 64;
				for (size_t page = 0; page < m_Copy->saved.size(); page += batchPages) {
					if (!m_Coordinator->CopyPages(page, std::min(batchPages, m_Copy->saved.size() - page)))
						throw std::runtime_error("could not open prepared Lua heap pages");
				}
				if (!m_Coordinator->Complete(m_Copy, true)) throw std::runtime_error("could not open a prepared Lua heap");
				m_Data->copied.store(m_Data->committed / Snapshot::c_PageBytes, std::memory_order_relaxed);
				m_Data->copyUs.store(MicrosecondsSince(started), std::memory_order_relaxed);
				m_Ready.set_value();
				m_Finished = true;
			} catch (...) {
				m_Coordinator->Cancel(m_Copy);
				m_Ready.set_exception(std::current_exception());
				m_Finished = true;
				throw;
			}
		}
		void Cancel() noexcept {
			std::lock_guard lock(m_Mutex);
			if (m_Finished) return;
			m_Coordinator->Cancel(m_Copy);
			m_Ready.set_exception(m_Cancelled);
			m_Finished = true;
		}
	private:
		friend class HeapOwner;
		Prepared(std::shared_ptr<Snapshot::Data> data, std::shared_ptr<CowCopy> copy, std::shared_ptr<CowCoordinator> coordinator) :
		    m_Data(std::move(data)), m_Copy(std::move(copy)), m_Coordinator(std::move(coordinator)),
		    m_Cancelled(std::make_exception_ptr(std::runtime_error("prepared Lua heap capture was cancelled"))) {
			m_Data->ready = m_Ready.get_future().share();
		}
		std::shared_ptr<Snapshot::Data> m_Data;
		std::shared_ptr<CowCopy> m_Copy;
		std::shared_ptr<CowCoordinator> m_Coordinator;
		std::promise<void> m_Ready;
		std::exception_ptr m_Cancelled;
		std::mutex m_Mutex;
		bool m_Finished = false;
	};

	inline std::shared_ptr<HeapOwner::Prepared> HeapOwner::PrepareFreeze() {
		std::lock_guard preparations(m_PrepareMutex);
		auto data = std::make_shared<Snapshot::Data>();
		data->state = m_State; data->handlers = m_LibraryHandlers; data->base = m_Base;
		data->committed = m_PublishedCommitted.load(std::memory_order_acquire);
		const size_t pageBytes = PageWriteFence::SystemPageBytes();
		if (!data->state || !pageBytes || !data->committed || data->base % pageBytes || data->committed % pageBytes)
			throw std::runtime_error("the prepared Lua heap has no aligned committed range");
		auto slab = TakeSlab(data->committed / Snapshot::c_PageBytes, true);
		data->pages = slab->pages; data->buffer = std::move(slab);
		data->faults = std::make_shared<CopyFaultStats>();
		auto copy = std::make_shared<CowCopy>();
		copy->base = data->base; copy->pageBytes = pageBytes;
		copy->destination = const_cast<Snapshot::Page*>(data->pages);
		copy->saved.assign(data->committed / pageBytes, 0);
		copy->faults = data->faults; copy->buffer = data->buffer;
		if (!m_CowCoordinator) {
			m_CowCoordinator = std::make_shared<CowCoordinator>();
			m_CowCoordinator->base = m_Base; m_CowCoordinator->pageBytes = pageBytes;
		}
		auto prepared = std::shared_ptr<Prepared>(new Prepared(std::move(data), std::move(copy), m_CowCoordinator));
		const auto keepWritable = [](void*, uintptr_t, size_t) noexcept { return true; };
		if (!PageWriteFence::WatchCopies(this, {reinterpret_cast<uint8_t*>(m_Base), prepared->m_Data->committed}, CowCoordinator::OnWrite, m_CowCoordinator.get(), keepWritable))
			throw std::runtime_error("could not prepare Lua heap page watches");
		m_CowCoordinator->Prepare(prepared->m_Copy);
		std::erase_if(m_Prepared, [](const auto& value) { return value.expired(); });
		m_Prepared.push_back(prepared);
		return prepared;
	}

	inline void HeapOwner::FinishPrepared() noexcept {
		// Closing a VM first lands armed readers and cancels preparations that never reached a boundary.
		for (const auto& weak: m_Prepared) if (auto prepared = weak.lock()) {
			try {
				if (prepared->m_Copy->armed.load(std::memory_order_acquire) == 1) prepared->Drain();
				else prepared->Cancel();
			} catch (...) { prepared->Cancel(); }
		}
		m_Prepared.clear();
	}

	inline HeapOwner::Slab::~Slab() {
		if (!pages) return;
		HeapOwner::Unmap(pages, capacity * Snapshot::c_PageBytes);
		HeapOwner::CopyBytes(false).fetch_sub(capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
	}

	inline bool HeapOwner::RunBatchOpenSelfTest() {
		const size_t page = PageWriteFence::SystemPageBytes(), bytes = page * 2;
		if (!page) return false;
		const auto release = [bytes](void* memory) { if (memory) Unmap(memory, bytes); };
		std::unique_ptr<void, decltype(release)> live(MapPages(bytes), release), firstBytes(MapPages(bytes), release), secondBytes(MapPages(bytes), release);
		if (!live || !firstBytes || !secondBytes) throw std::bad_alloc();
		auto* source = static_cast<unsigned char*>(live.get());
		std::memset(source, 3, bytes);
		CowCoordinator coordinator;
		coordinator.base = reinterpret_cast<uintptr_t>(source); coordinator.pageBytes = page;
		struct Observer { CowCoordinator& coordinator; size_t faults = 0; } observer{coordinator};
		const auto observe = [](void* context, uintptr_t address) noexcept {
			auto& value = *static_cast<Observer*>(context);
			++value.faults;
			return value.coordinator.CopyPage((address - value.coordinator.base) / value.coordinator.pageBytes, true);
		};
		struct Unwatch { void* owner; ~Unwatch() { PageWriteFence::UnwatchCopies(owner); } } unwatch{&observer};
		const auto prepare = [&](void* destination) {
			auto copy = std::make_shared<CowCopy>();
			copy->base = coordinator.base; copy->pageBytes = page; copy->destination = static_cast<Snapshot::Page*>(destination);
			copy->saved.assign(2, 0); copy->faults = std::make_shared<CopyFaultStats>();
			coordinator.Prepare(copy);
			CowCoordinator::Prepared prepared{&coordinator, copy};
			if (!PageWriteFence::WatchCopies(&observer, {source, bytes}, observe, &observer, CowCoordinator::Arm, &prepared)) return std::shared_ptr<CowCopy>();
			return copy;
		};
		const auto first = prepare(firstBytes.get());
		if (!first || !coordinator.CopyPages(0, 1)) return false;
		source[0] = 5;
		if (observer.faults) return false;
		source[page] = 7;
		if (observer.faults != 1) return false;
		const auto second = prepare(secondBytes.get());
		if (!second || !coordinator.CopyPages(0, 1)) return false;
		source[0] = 9;
		if (observer.faults != 1) return false;
		source[page] = 13;
		if (observer.faults != 2 || !coordinator.Complete(first, true) || !coordinator.Complete(second, true)) return false;
		const auto* old = static_cast<const unsigned char*>(firstBytes.get());
		const auto* next = static_cast<const unsigned char*>(secondBytes.get());
		return old[0] == 3 && old[page] == 3 && next[0] == 5 && next[page] == 7 && source[0] == 9 && source[page] == 13;
	}

	inline bool HeapOwner::RunPreparedSelfTest() {
		auto owner = Create();
		lua_pushnumber(owner->State(), 11);
		TValue* value = owner->State()->top - 1;
		auto prepared = owner->PrepareFreeze();
		setnumV(value, 23);
		{
			CheckpointFailure::Scope failure(CheckpointFailure::Point::LuaPages);
			if (!prepared->Arm(*owner) || failure.Triggered()) return false;
		}
		const Snapshot first = prepared->Image();
		setnumV(value, 37);
		prepared->Drain();
		if (first.Read(value).n != 23 || value->n != 37) return false;
		prepared.reset();
		{
			auto cancelled = owner->PrepareFreeze();
			const Snapshot abandoned = cancelled->Image();
			cancelled->Cancel();
			if (cancelled->Arm(*owner)) return false;
			bool refused = false;
			try { abandoned.Read(value); } catch (const std::runtime_error&) { refused = true; }
			if (!refused) return false;
		}
		auto stale = owner->PrepareFreeze();
		if (!owner->Bump(owner->m_Committed - owner->m_Used + Snapshot::c_PageBytes)) throw std::bad_alloc();
		if (stale->Current(*owner) || stale->Arm(*owner)) return false;
		stale.reset();
		bool refused = false;
		{
			CheckpointFailure::Scope failure(CheckpointFailure::Point::LuaPages);
			try { owner->PrepareFreeze(); } catch (const std::bad_alloc&) { refused = failure.Triggered(); }
		}
		if (!refused || value->n != 37) return false;
		auto retry = owner->PrepareFreeze();
		{
			CheckpointFailure::Scope failure(CheckpointFailure::Point::CopyWatch);
			if (!retry->Arm(*owner) || failure.Triggered()) return false;
		}
		const Snapshot second = retry->Image();
		setnumV(value, 59);
		// Source destruction must complete a reader even when its saver has not started yet.
		owner.reset();
		retry->Drain();
		return first.Read(value).n == 23 && second.Read(value).n == 37;
	}

	inline bool HeapOwner::RunReusedOwnerSelfTest() {
		alignas(HeapOwner) std::byte storage[sizeof(HeapOwner)];
		const auto create = [&] {
			const auto destroy = [](HeapOwner* owner) { owner->~HeapOwner(); };
			std::unique_ptr<HeapOwner, decltype(destroy)> owner(new (storage) HeapOwner, destroy);
			owner->Initialize();
			std::lock_guard lock(RegistryMutex());
			LiveHeaps().emplace(owner.get(), owner->m_Identity);
			return owner;
		};
		auto firstOwner = create();
		auto prepared = firstOwner->PrepareFreeze();
		if (!prepared->Arm(*firstOwner)) return false;
		Snapshot retained = prepared->Image();
		firstOwner.reset();
		prepared.reset();
		const size_t retainedBytes = retained.ByteCount(), before = MappedCopyBytes();
		auto replacement = create();
		retained = {};
		return replacement->LiveSlabs() == 0 && MappedCopyBytes() + retainedBytes == before;
	}

	// Every userdata hangs after the main thread in the GC chain; nothing before it is one.
	template<class Visit> void ForEachUserdata(lua_State* state, bool includeFinalized, bool includeQueued, Visit visit) {
		global_State* global = G(state);
		for (GCobj* object = gcnext(obj2gco(mainthread(global))); object; object = gcnext(object)) {
			if (object->gch.gct != ~LJ_TUDATA || (!includeFinalized && (object->gch.marked & LJ_GC_FINALIZED))) continue;
			visit(gco2ud(object));
		}
		// Queued finalizers have the finalized bit set, but their native payload is still alive.
		if (GCobj* last = includeQueued ? gcref(global->gc.mmudata) : nullptr) {
			GCobj* object = last;
			do {
				object = gcnext(object);
				if (object->gch.gct == ~LJ_TUDATA) visit(gco2ud(object));
			} while (object != last);
		}
	}

	// The one rule every capture walks userdata by: a finalizer that already ran left its native payload
	// destroyed, and a queued one has not run yet, so the first is skipped and the second is visited.
	template<class Visit> void ForEachCapturedUserdata(lua_State* state, Visit visit) {
		ForEachUserdata(state, false, true, visit);
	}
}
