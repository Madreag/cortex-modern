#pragma once

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_vmevent.h"
}

#include "CaptureSentinel.h"

#include <algorithm>
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
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/mman.h>
#endif

namespace RTE::CheckpointLua {

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

	// The VM's memory at one freeze: a copy of every committed page. Addresses identify the source VM; only the copy is read.
	class Snapshot {
	public:
		static constexpr size_t c_PageBytes = 4096;
		struct Page { std::byte bytes[c_PageBytes]; };

		Snapshot() = default;
		lua_State* State() const { return m_Data ? m_Data->state : nullptr; }
		uint64_t StateSerial() const { return m_Data ? m_Data->serial : 0; }
		size_t ByteCount() const { return m_Data ? m_Data->committed : 0; }
		size_t BlockCount() const { return m_Data ? m_Data->copied.load(std::memory_order_relaxed) : 0; }
		int64_t FreezeUs() const { return m_Data ? m_Data->freezeUs : 0; }
		int64_t CopyUs() const { return m_Data ? m_Data->copyUs.load(std::memory_order_relaxed) : 0; }
		size_t FaultCount() const { return 0; }
		size_t PreviousFaults() const { return 0; }
		int64_t PreviousFaultUs() const { return 0; }

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
			const size_t first = offset / c_PageBytes, within = offset % c_PageBytes;
			if (within + size <= c_PageBytes) return {PageBytes(first) + within, size};
			// A read across pages is assembled once and kept with the snapshot.
			auto& buffer = m_Data->assembled.emplace_back(size);
			size_t done = 0;
			for (size_t page = first, at = within; done < size; ++page, at = 0) {
				const size_t chunk = std::min(size - done, c_PageBytes - at);
				std::memcpy(buffer.data() + done, PageBytes(page) + at, chunk);
				done += chunk;
			}
			return {buffer.data(), size};
		}

		std::string ReadString(const char* address, size_t size) const {
			const auto bytes = ReadBytes(address, size);
			return size ? std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()) : std::string();
		}

	private:
		struct Data {
			lua_State* state = nullptr;
			uint64_t serial = 0;
			uintptr_t base = 0;
			size_t committed = 0;
			std::atomic<size_t> copied{0};
			int64_t freezeUs = 0;
			std::atomic<int64_t> copyUs{0};
			std::shared_ptr<const void> buffer; // Keeps the copy mapped.
			const Page* pages = nullptr; // One per committed page, in order.
			std::shared_future<void> ready; // Set only when a freeze was given somewhere to run its copy.
			mutable std::atomic<bool> copiedFlag{false};
			mutable std::deque<std::vector<std::byte>> assembled; // Read by the one worker that walks this snapshot.
			void WaitCopied() const {
				if (copiedFlag.load(std::memory_order_acquire)) return;
				// A copy that failed left pages unread, so its reader fails with it instead of reading zeros.
				if (ready.valid()) ready.get();
				copiedFlag.store(true, std::memory_order_release);
			}
		};
		std::shared_ptr<const Data> m_Data;
		explicit Snapshot(std::shared_ptr<const Data> data) : m_Data(std::move(data)) {}

		const std::byte* PageBytes(size_t index) const { return m_Data->pages[index].bytes; }

		friend class HeapOwner;
	};

	// Threads that copy frozen heaps while the thread that froze them runs on. A task never enters a VM.
	class CopyPool {
	public:
		static std::future<void> Submit(std::function<void()> work) {
			static CopyPool* pool = new CopyPool(); // Never destroyed, so a heap released at exit still gets its copy.
			return pool->Push(std::move(work));
		}

	private:
		std::mutex m_Mutex;
		std::condition_variable m_Ready;
		std::deque<std::packaged_task<void()>> m_Tasks;

		CopyPool() {
			const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
			for (unsigned index = 0; index < threads; ++index) {
				std::thread([this] {
					while (true) {
						std::packaged_task<void()> task;
						{
							std::unique_lock lock(m_Mutex);
							m_Ready.wait(lock, [this] { return !m_Tasks.empty(); });
							task = std::move(m_Tasks.front());
							m_Tasks.pop_front();
						}
						CaptureSentinel::WorkerScope worker("heap-copy");
						task();
					}
				}).detach();
			}
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
	class HeapOwner {
	public:
		static std::unique_ptr<HeapOwner> Create() {
			auto owner = std::unique_ptr<HeapOwner>(new HeapOwner());
			owner->Initialize();
			{
				std::lock_guard lock(RegistryMutex());
				LiveHeaps().insert(owner.get());
			}
			return owner;
		}

		~HeapOwner() {
			WaitCopy();
			// From here a buffer released anywhere, by a snapshot that outlives this heap too, unmaps itself.
			{
				std::lock_guard lock(RegistryMutex());
				LiveHeaps().erase(this);
			}
			{
				std::lock_guard lock(m_SlabMutex);
				for (const auto& slab: m_IdleSlabs) {
					CopyBytes(true).fetch_sub(slab->capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
					slab->owner = nullptr;
				}
				m_IdleSlabs.clear();
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

		using Submit = std::function<std::future<void>(std::function<void()>)>;

		// With a Submit the freeze only fixes the instant and the copy runs there: no VM may run until it lands,
		// and every way into this VM waits for it at the gate (WaitCopy, from the state's lock and from the
		// allocator), so the copy never needs the VM's own lock. Without a Submit the copy runs here, on the caller's thread.
		Snapshot Freeze(const Submit& submit) {
			const auto started = std::chrono::steady_clock::now();
			if (!m_State) throw std::runtime_error("a Lua heap capture has no state");
			void* allocatorData = nullptr;
			if (lua_getallocf(m_State, &allocatorData) != &Allocate || allocatorData != this)
				throw std::runtime_error("the Lua heap allocator changed after tracking began");
			WaitCopy();
			auto data = std::make_shared<Snapshot::Data>();
			data->state = m_State;
			data->serial = G(m_State)->objserial;
			data->base = m_Base;
			data->committed = m_Committed;
			const bool receipt = Receipts().load(std::memory_order_acquire);
			auto copy = [this, data, started, receipt] {
				m_FreshBytes = 0;
				CopyPages(*data);
				if (!receipt) return;
				std::lock_guard lock(m_CopyMutex);
				m_LandedCopy = {++m_LandedGeneration, data->copied.load(std::memory_order_relaxed), data->copyUs.load(std::memory_order_relaxed), m_FreshBytes, MicrosecondsSince(started)};
				LandedCopies().fetch_add(1, std::memory_order_release);
			};
			if (submit) {
				// The snapshot waits on a promise of its own: a task's future can keep the task, and with it this
				// snapshot, alive for as long as the snapshot holds that future.
				auto done = std::make_shared<std::promise<void>>();
				std::lock_guard lock(m_CopyMutex);
				data->ready = done->get_future().share();
				m_PendingCopy = data->ready;
				++m_CopyGeneration;
				m_CopyPending.store(true, std::memory_order_release);
				submit([copy = std::move(copy), done] {
					try {
						copy();
						done->set_value();
					} catch (...) {
						done->set_exception(std::current_exception());
					}
				});
			} else {
				copy();
			}
			data->freezeUs = MicrosecondsSince(started);
			return Snapshot(std::move(data));
		}

		// The gate: blocks until the last freeze's copy has landed; the VM may write its heap again after this.
		void WaitCopy() {
			if (!m_CopyPending.load(std::memory_order_acquire)) return;
			std::unique_lock lock(m_CopyMutex);
			if (!m_PendingCopy.valid()) return;
			const std::shared_future<void> pending = m_PendingCopy;
			const uint64_t generation = m_CopyGeneration;
			lock.unlock();
			if (pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
				const auto waited = std::chrono::steady_clock::now();
				pending.wait();
				const int64_t waitedUs = MicrosecondsSince(waited);
				GateWaitUs().fetch_add(waitedUs, std::memory_order_relaxed);
				ThreadGateWaitUs() += waitedUs;
			}
			lock.lock();
			if (generation == m_CopyGeneration) {
				m_PendingCopy = {};
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
		std::unique_ptr<lua_State, decltype(&lua_close)> m_Bootstrap{nullptr, lua_close};
		lua_State* m_State = nullptr;
		uintptr_t m_Reservation = 0;
		uintptr_t m_Base = 0;
		size_t m_Committed = 0;
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
			~Slab();
		};
		static constexpr size_t c_IdleSlabs = 1; // With one capture in flight, the next freeze reuses the buffer the last one gave back.
		std::mutex m_SlabMutex;
		std::vector<std::unique_ptr<Slab>> m_IdleSlabs;
		std::atomic<size_t> m_LiveSlabs{0};
		std::mutex m_CopyMutex;
		std::shared_future<void> m_PendingCopy;
		uint64_t m_CopyGeneration = 0;
		std::atomic<bool> m_CopyPending{false};
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
		static std::unordered_set<const HeapOwner*>& LiveHeaps() {
			static auto* heaps = new std::unordered_set<const HeapOwner*>();
			return *heaps;
		}
		static std::atomic<size_t>& DeadHeapCalls() {
			static std::atomic<size_t> calls{0};
			return calls;
		}
		std::shared_ptr<Slab> TakeSlab(size_t pages) {
			std::unique_ptr<Slab> slab;
			{
				std::lock_guard lock(m_SlabMutex);
				const auto fit = std::find_if(m_IdleSlabs.begin(), m_IdleSlabs.end(), [pages](const auto& candidate) { return candidate->capacity >= pages; });
				if (fit != m_IdleSlabs.end()) {
					slab = std::move(*fit);
					m_IdleSlabs.erase(fit);
					CopyBytes(true).fetch_sub(slab->capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
				}
			}
			if (!slab) {
				slab = std::make_unique<Slab>();
				slab->owner = this;
				// Room for the heap to grow before a freeze needs a bigger buffer.
				slab->capacity = pages + pages / 4;
				slab->pages = static_cast<Snapshot::Page*>(MapPages(slab->capacity * Snapshot::c_PageBytes));
				if (!slab->pages) throw std::runtime_error("could not map a Lua heap copy");
				m_FreshBytes += slab->capacity * Snapshot::c_PageBytes;
				CopyBytes(false).fetch_add(slab->capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
			}
			m_LiveSlabs.fetch_add(1, std::memory_order_relaxed);
			return std::shared_ptr<Slab>(std::move(slab));
		}
		// A buffer goes back to its heap only while that heap exists; one released after it is unmapped instead.
		static void ReturnSlab(HeapOwner* owner, Slab* slab) {
			{
				std::lock_guard lock(RegistryMutex());
				if (LiveHeaps().contains(owner)) {
					owner->GiveSlab(slab);
					return;
				}
			}
			slab->owner = nullptr;
			delete slab;
		}
		// The caller holds RegistryMutex.
		void GiveSlab(Slab* slab) {
			if (!LiveHeaps().contains(this)) {
				// Only the address is read: the heap behind it is gone, so the buffer is left mapped rather than touch it.
				DeadHeapCalls().fetch_add(1, std::memory_order_relaxed);
				return;
			}
			m_LiveSlabs.fetch_sub(1, std::memory_order_relaxed);
			std::lock_guard lock(m_SlabMutex);
			// The next freeze needs a buffer as big as the heap, so a full pool keeps its largest.
			if (m_IdleSlabs.size() >= c_IdleSlabs) {
				const auto smallest = std::min_element(m_IdleSlabs.begin(), m_IdleSlabs.end(), [](const auto& a, const auto& b) { return a->capacity < b->capacity; });
				if ((*smallest)->capacity >= slab->capacity) {
					DropSlab(slab);
					return;
				}
				CopyBytes(true).fetch_sub((*smallest)->capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
				DropSlab(smallest->release());
				m_IdleSlabs.erase(smallest);
			}
			m_IdleSlabs.push_back(std::unique_ptr<Slab>(slab));
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
#ifndef LUAJIT_DISABLE_VMEVENT
			lua_getfield(m_Bootstrap.get(), LUA_REGISTRYINDEX, LJ_VMEVENTS_REGKEY);
			if (!lua_istable(m_Bootstrap.get(), -1)) throw std::runtime_error("the Lua bootstrap supplied no finalizer event table");
			lua_rawgeti(m_Bootstrap.get(), -1, VMEVENT_HASH(LJ_VMEVENT_ERRFIN));
			const lua_CFunction finalizerError = lua_tocfunction(m_Bootstrap.get(), -1);
			if (!finalizerError || lua_getupvalue(m_Bootstrap.get(), -1, 1))
				throw std::runtime_error("the Lua bootstrap finalizer handler cannot be transferred");
			lua_pop(m_Bootstrap.get(), 2);
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

	// The last page of a slab released gives the buffer back to the pool of its owner.
	inline HeapOwner::Slab::~Slab() {
		if (!pages) return;
		if (owner) {
			auto* kept = new Slab();
			kept->pages = std::exchange(pages, nullptr);
			kept->capacity = capacity;
			kept->owner = owner;
			HeapOwner::ReturnSlab(owner, kept);
			return;
		}
		HeapOwner::Unmap(pages, capacity * Snapshot::c_PageBytes);
		HeapOwner::CopyBytes(false).fetch_sub(capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
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
