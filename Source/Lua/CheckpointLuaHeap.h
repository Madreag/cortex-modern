#pragma once

#include "CheckpointNativeStorage.h"
#include "SteadyClock.h"

#include "FloatingPointEnvironment.h"
#include "ThreadMan.h"
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
#include "CheckpointPageCopy.h"

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
		// What the boundary itself copied of the heap, and its time.
		int64_t armCopyUs = 0;
		size_t armCopiedPages = 0;
		bool armWhole = false;
	};

	struct LibraryHandlers {
		lua_CFunction panic = nullptr, finalizerError = nullptr;
		uint8_t eventMask = 0;
	};

	// Saver-created descriptor tables hold frozen tokens without putting them in a running VM.
	struct ScratchData {
	private:
		friend class Snapshot;
		friend class ScratchImage;
		struct Block {
			std::unique_ptr<std::byte[]> bytes;
			size_t size = 0;
			size_t used = 0;
		};
		std::vector<Block> blocks;
		std::vector<const GCobj*> objects;
		bool sealed = false;
		const std::byte* Find(const void* address, size_t bytes) const {
			const auto pointer = reinterpret_cast<uintptr_t>(address);
			for (const auto& block: blocks) {
				const auto begin = reinterpret_cast<uintptr_t>(block.bytes.get());
				if (pointer >= begin && bytes <= block.used && pointer - begin <= block.used - bytes) return block.bytes.get() + (pointer - begin);
			}
			return nullptr;
		}
	};

	// The VM's memory at one freeze: a copy of every committed page. Addresses identify the source VM; only the copy is read.
	class Snapshot {
	public:
		using Reader = std::function<std::span<const std::byte>(const void*, size_t)>;
		static constexpr size_t c_PageBytes = 4096;
		struct Page { std::byte bytes[c_PageBytes]; };

		Snapshot() = default;
		lua_State* State() const { return m_Data ? m_Data->state : nullptr; }
		LibraryHandlers Handlers() const { return m_Data ? m_Data->handlers : LibraryHandlers{}; }
		uint64_t StateSerial() const { return m_Data ? m_Data->serial : 0; }
		size_t ByteCount() const { return m_Data ? m_Data->committed : 0; }
		bool OriginalBytes(const void* address, size_t bytes) const {
			const auto pointer = reinterpret_cast<uintptr_t>(address);
			return m_Data && pointer >= m_Data->base && bytes <= m_Data->committed && pointer - m_Data->base <= m_Data->committed - bytes;
		}
		std::pair<const void*, std::span<const std::byte>> NativeBytes() const {
			if (!m_Data) throw std::logic_error("a Lua heap snapshot is empty");
			m_Data->WaitCopied();
			return {reinterpret_cast<const void*>(m_Data->base), {reinterpret_cast<const std::byte*>(m_Data->pages), m_Data->committed}};
		}
		size_t BlockCount() const { return m_Data ? m_Data->copied.load(std::memory_order_relaxed) : 0; }
		int64_t FreezeUs() const { return m_Data ? m_Data->freezeUs : 0; }
		int64_t CopyUs() const { return m_Data ? m_Data->copyUs.load(std::memory_order_relaxed) : 0; }
		size_t FaultCount() const { return m_Data && m_Data->faults ? m_Data->faults->count.load(std::memory_order_relaxed) : 0; }
		int64_t ArmCopyUs() const { return m_Data && m_Data->faults ? m_Data->faults->armCopyUs : 0; }
		size_t ArmCopiedPages() const { return m_Data && m_Data->faults ? m_Data->faults->armCopiedPages : 0; }
		bool ArmWhole() const { return m_Data && m_Data->faults && m_Data->faults->armWhole; }
		/// The bytes the state allocated between its two full collections before the heap was prepared.
		size_t PeriodAllocated() const { return m_Data ? m_Data->periodAllocated : 0; }
		/// Whether the state allocated enough of its heap between collections that the ticks after a boundary write a large
		/// share of its pages: a busy heap is copied whole at the boundary.
		bool Busy() const { return m_Data && m_Data->busy; }
		size_t PreviousFaults() const { return m_Data ? m_Data->previousFaults : 0; }
		int64_t PreviousFaultUs() const { return m_Data ? m_Data->previousFaultUs : 0; }

		template<class T> T Read(const T* address) const {
			static_assert(std::is_trivially_copyable_v<T>);
			const auto bytes = ReadBytes(address, sizeof(T));
			T value;
			std::memcpy(&value, bytes.data(), sizeof(value));
			return value;
		}

		// Frozen views stay with the snapshot; private getter views last only until their VM closes.
		std::span<const std::byte> ReadBytes(const void* address, size_t size) const {
			if (!m_Data) throw std::runtime_error("a Lua heap snapshot is empty");
			if (size == 0) return {};
			const uintptr_t source = reinterpret_cast<uintptr_t>(address);
			if (source >= m_Data->base && size <= m_Data->committed && source - m_Data->base <= m_Data->committed - size) {
				m_Data->WaitCopied();
				return {reinterpret_cast<const std::byte*>(m_Data->pages) + (source - m_Data->base), size};
			}
			for (const auto& values: m_Scratch) if (const auto* bytes = values->Find(address, size)) return {bytes, size};
			for (const auto& reader: m_Private) if (const auto bytes = reader(address, size); !bytes.empty()) return bytes;
			throw std::runtime_error("a Lua heap read lies outside the recorded heap");
		}
		Snapshot WithPrivate(Reader reader) const {
			if (!m_Data || !reader) throw std::logic_error("a saved getter needs a recorded heap and private reader");
			Snapshot result = *this;
			result.m_Private.push_back(std::move(reader));
			return result;
		}
		Snapshot WithScratch(std::shared_ptr<const ScratchData> values) const {
			if (!m_Data || !values || !values->sealed) throw std::logic_error("unfinished frozen Lua descriptors");
			for (const auto& block: values->blocks) {
				const auto begin = reinterpret_cast<uintptr_t>(block.bytes.get());
				if (begin < m_Data->base + m_Data->committed && m_Data->base < begin + block.size)
					throw std::runtime_error("frozen Lua descriptor storage reused a source address");
			}
			Snapshot result = *this;
			result.m_Scratch.push_back(std::move(values));
			return result;
		}
		template<class Visitor> void VisitScratchObjects(Visitor visit) const {
			for (const auto& values: m_Scratch) for (const auto* object: values->objects) visit(object);
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
			if (reference < 0) { TValue nil; setnilV(&nil); return nil; }
			const auto state = Read(State());
			const auto globals = Read(mref(state.glref, global_State));
			if (!tvistab(&globals.registrytv)) throw std::runtime_error("a frozen Lua registry is not a table");
			return TableValue(tabV(&globals.registrytv), reference);
		}
		void* LightUserdata(const TValue& value) const {
			if (!tvislightud(&value)) throw std::runtime_error("a frozen lightuserdata value has the wrong type");
#if LJ_64
			const auto state = Read(State());
			const auto globals = Read(mref(state.glref, global_State));
			const uint64_t segment = lightudseg(value.u64);
			if (segment == (1 << LJ_LIGHTUD_BITS_SEG) - 1) return nullptr;
			if (segment > globals.gc.lightudnum) throw std::runtime_error("invalid frozen lightuserdata segment");
			const uint32_t upper = Read(mref(globals.gc.lightudseg, uint32_t) + segment);
			return reinterpret_cast<void*>((static_cast<uint64_t>(upper) << 32) | lightudlo(value.u64));
#else
			return gcrefp(value.gcr, void);
#endif
		}
		TValue RegistryLightUserdataValue(const void* key) const {
			const auto state = Read(State());
			const auto globals = Read(mref(state.glref, global_State));
			if (!tvistab(&globals.registrytv)) throw std::runtime_error("a frozen Lua registry is not a table");
			const auto table = Read(tabV(&globals.registrytv));
			const Node* nodes = mref(table.node, Node);
			for (MSize index = 0; index <= table.hmask; ++index) {
				const auto node = Read(nodes + index);
				if (!tvisnil(&node.val) && tvislightud(&node.key) && LightUserdata(node.key) == key) return node.val;
			}
			TValue nil; setnilV(&nil); return nil;
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
			size_t periodAllocated = 0;
			bool busy = false;
			std::atomic<int64_t> copyUs{0};
			std::shared_ptr<CopyFaultStats> faults;
			size_t previousFaults = 0;
			int64_t previousFaultUs = 0;
			std::shared_ptr<const void> buffer; // Keeps the copy mapped.
			std::shared_ptr<const void> sourceMemory;
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
		std::vector<std::shared_ptr<const ScratchData>> m_Scratch;
		std::vector<Reader> m_Private;
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
						SaverThreads::Mark();
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
		struct PreparationLife {
			std::mutex mutex;
			HeapOwner* owner = nullptr;
		};
	public:
		class Prepared;
		class ArmSession;
		/// A queued preparation keeps its original owner identity across VM closure and address reuse.
		class PreparationLease {
		public:
			PreparationLease() = default;
			std::shared_ptr<Prepared> Prepare() const;
			bool Current(const Prepared& prepared) const noexcept;
			bool Arm(Prepared& prepared, bool byCopy = false) const noexcept;
			/// The boundary's arm on several threads: Arm's locks and checks into the session, and the pages to copy.
			bool BeginArm(Prepared& prepared, bool whole, bool learn, ArmSession& session) const;
			explicit operator bool() const noexcept { return m_Life != nullptr; }
		private:
			friend class HeapOwner;
			explicit PreparationLease(std::shared_ptr<PreparationLife> life) : m_Life(std::move(life)) {}
			std::shared_ptr<PreparationLife> m_Life;
		};
		static PreparationLease LeasePreparation(lua_State* state) {
			std::lock_guard lock(RegistryMutex());
			const auto found = StatePreparations().find(state);
			return found == StatePreparations().end() ? PreparationLease{} : PreparationLease(found->second.lock());
		}
		static std::unique_ptr<HeapOwner> Create() {
			auto owner = std::unique_ptr<HeapOwner>(new HeapOwner());
			owner->Initialize();
			owner->m_PreparationLife->owner = owner.get();
			{
				std::lock_guard lock(RegistryMutex());
				LiveHeaps().emplace(owner.get(), owner->m_Identity);
				StatePreparations().emplace(owner->m_State, owner->m_PreparationLife);
			}
			return owner;
		}

		~HeapOwner() {
			std::lock_guard alive(m_PreparationLife->mutex);
			m_PreparationLife->owner = nullptr;
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
				StatePreparations().erase(m_State);
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
			if (m_PrivateRead) m_PrivateRead->base.store(0, std::memory_order_release);
			for (const auto& [begin, bytes]: m_PrivateChunks) CheckpointNativeStorage::ForgetPrivate(reinterpret_cast<void*>(begin));
			m_Bootstrap.reset();
			if (m_Reservation) HeapCommitted().fetch_sub(m_Committed, std::memory_order_relaxed);
			Release();
		}
		HeapOwner(const HeapOwner&) = delete;
		HeapOwner& operator=(const HeapOwner&) = delete;
		HeapOwner(HeapOwner&&) = delete;
		HeapOwner& operator=(HeapOwner&&) = delete;

		lua_State* State() const { return m_State; }
		Snapshot::Reader PrivateReader() const {
			if (!m_PrivateRead) {
				m_PrivateRead = std::make_shared<PrivateReadLife>();
				m_PrivateRead->committed.store(m_Committed, std::memory_order_release);
				m_PrivateRead->base.store(m_Base, std::memory_order_release);
			}
			return [life = m_PrivateRead](const void* address, size_t bytes) -> std::span<const std::byte> {
				if (life->thread != std::this_thread::get_id()) throw std::logic_error("a saved getter heap belongs to its saver thread");
				const uintptr_t base = life->base.load(std::memory_order_acquire);
				const size_t committed = life->committed.load(std::memory_order_acquire);
				const uintptr_t at = reinterpret_cast<uintptr_t>(address);
				if (base && at >= base && bytes <= committed && at - base <= committed - bytes)
					return {reinterpret_cast<const std::byte*>(address), bytes};
				return {};
			};
		}
		CheckpointNativeStorage::RootRange PrivateNativeRange() const {
			if (!m_PrivateNative) throw std::logic_error("only a saved getter VM owns private native heap values");
			const auto* base = reinterpret_cast<const void*>(m_Base);
			return {base, base, m_Committed, true};
		}

		AllocationStats Stats() const { return {m_Bytes, m_Blocks}; }

		/// Every copy buffer any heap has mapped, idle ones included, and the idle part of it.
		static size_t MappedCopyBytes() { return CopyBytes(false).load(std::memory_order_relaxed); }
		/// The copy buffers' committed bytes: those a capture is using or about to use.
		static size_t CommittedCopyBytes() { return SlabCommitted().load(std::memory_order_relaxed); }
		/// Whether a capture is near, so a copy buffer given back keeps its memory for the next one.
		static void KeepCopiesWarm(bool warm) noexcept { KeepWarm().store(warm, std::memory_order_release); }
		/// Every live heap's committed pages.
		static size_t CommittedHeapBytes() { return HeapCommitted().load(std::memory_order_relaxed); }
		static size_t IdleCopyBytes() { return CopyBytes(true).load(std::memory_order_relaxed); }
		/// The copy buffers of this heap that a snapshot still holds.
		size_t LiveSlabs() const { return m_LiveSlabs.load(std::memory_order_relaxed); }
		/// The buffer a snapshot holds and the one the next freeze fills while it still holds it.
		static constexpr size_t c_LiveSlabs = 2;
		/// Buffers handed back to a heap that no longer exists; any is a use after free.
		static size_t CallsIntoDestroyedHeaps() { return DeadHeapCalls().load(std::memory_order_relaxed); }
		static bool RunBatchOpenSelfTest();
		static bool RunLearningArmSelfTest();
		static bool RunPreparedSelfTest();
		static bool RunPrivateNativeSelfTest();
		static bool RunPreparationLifetimeSelfTest();
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
			const auto started = RTE::SteadyClock::now();
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
			data->sourceMemory = m_HeapMemory;
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
					const auto copying = RTE::SteadyClock::now();
					// Bounded page runs avoid repeated protection calls while live faults still save individual pages.
					const size_t batchPages = batchCopy ? 64 : 1;
					for (size_t page = 0; page < cow->saved.size(); page += batchPages) {
						const bool opened = batchCopy ? coordinator->CopyPages(page, std::min(batchPages, cow->saved.size() - page))
						                             : coordinator->CopyPage(page, false);
						if (!opened)
							throw std::runtime_error("could not open copied Lua heap pages");
						if (SaverThreads::Current()) coordinator->TouchPages(page, std::min(batchPages, cow->saved.size() - page));
						SaverThreads::YieldToGame();
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
					auto submitted = submit([copy = std::move(copy), done, cow = m_CowCopy, coordinator = m_CowCoordinator] {
						try {
							copy();
							done->set_value();
						} catch (...) {
							if (cow && coordinator) coordinator->Cancel(cow);
							done->set_exception(std::current_exception());
						}
					});
					// A deferred copy must run before its submission future is released.
					if (submitted.valid() && submitted.wait_for(std::chrono::seconds(0)) == std::future_status::deferred) submitted.get();
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
					const auto waited = RTE::SteadyClock::now();
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
		struct PrivateReadLife {
			const std::thread::id thread = std::this_thread::get_id();
			std::atomic<uintptr_t> base{0};
			std::atomic<size_t> committed{0};
		};
		mutable std::shared_ptr<PrivateReadLife> m_PrivateRead;
		const bool m_PrivateNative = CheckpointNativeStorage::Reading();
		const bool m_YieldsToGame = SaverThreads::Current(); //!< A saver's own worker state stops for the game's ticks.
		std::vector<std::pair<uintptr_t, size_t>> m_PrivateChunks;
		std::shared_ptr<PreparationLife> m_PreparationLife = std::make_shared<PreparationLife>();
		std::unique_ptr<lua_State, decltype(&lua_close)> m_Bootstrap{nullptr, lua_close};
		lua_State* m_State = nullptr;
		LibraryHandlers m_LibraryHandlers;
		uintptr_t m_Reservation = 0;
		std::shared_ptr<void> m_HeapMemory;
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
			std::atomic<bool> committed{true};
			/// Idempotent and safe beside another commit of the same buffer.
			bool Commit() noexcept;
			/// Only while nothing can write the buffer.
			void Decommit() noexcept;
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
		std::atomic<size_t> m_PeriodAllocated{0};
		std::atomic<bool> m_Busy{false};
		size_t m_LastCollectedBytes = 0;
		static inline thread_local bool t_Collecting = false;
		void FinishPrepared() noexcept;
	public:
		/// Test lever CCCP_TEST_FAULT_TICKS: the Lua heap write faults taken, and their time.
		static bool TallyingFaults() {
			static const bool tallying = std::getenv("CCCP_TEST_FAULT_TICKS") != nullptr;
			return tallying;
		}
		/// Test lever CCCP_TEST_FAULT_TICKS: this heap's write faults since the last call, by its scripts and by its collector,
		/// the runs its last boundary copied, the hot runs recorded since, and 1 when that boundary copied the heap whole.
		std::array<uint32_t, 5> TakeTickFaults() noexcept;
		/// Test lever CCCP_TEST_FAULT_TICKS: once, at the end of the first tick after the heap's last boundary, the pages its
		/// copy holds and how many of them the game has written since (live bytes that differ from the copy); else {0, 0}.
		std::array<uint32_t, 2> TakeFirstTickWrites() noexcept;
		/// The state's full collection: what it allocated since the one before is how widely its scripts write its heap, since
		/// a state that allocates much reuses freed memory all over it. A heap turns busy at a sixteenth of its committed bytes
		/// allocated in a period and stays busy until a period allocates less than a sixty-fourth, so a busy state whose rate
		/// settles keeps being copied whole instead of being fenced with runs it never had the chance to record.
		void NoteFullCollection(size_t before, size_t after) noexcept {
			const size_t allocated = before > m_LastCollectedBytes ? before - m_LastCollectedBytes : 0;
			const size_t committed = m_PublishedCommitted.load(std::memory_order_acquire);
			if (m_LastCollectedBytes && committed) {
				m_PeriodAllocated.store(allocated, std::memory_order_relaxed);
				if (allocated * 16 >= committed) m_Busy.store(true, std::memory_order_relaxed);
				else if (allocated * 64 < committed) m_Busy.store(false, std::memory_order_relaxed);
			}
			m_LastCollectedBytes = after;
		}
		static std::pair<uint64_t, uint64_t> TakeFaultTally() { return {FaultTally()[0].exchange(0), FaultTally()[1].exchange(0)}; }
		static std::array<std::atomic<uint64_t>, 2>& FaultTally() {
			static std::array<std::atomic<uint64_t>, 2> tally{};
			return tally;
		}
		/// Marks the calling thread's full collection of a state: its writes cover the whole heap once and say nothing about
		/// what the ticks after the next boundary write, so they never join a heap's hot runs.
		struct CollectionScope {
			CollectionScope() noexcept { t_Collecting = true; }
			~CollectionScope() { t_Collecting = false; }
			CollectionScope(const CollectionScope&) = delete;
			CollectionScope& operator=(const CollectionScope&) = delete;
		};
	private:
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
			// The runs written in the ticks after a boundary, which the ticks after the next boundary write again: that
			// boundary copies them instead of fencing them. A copied run cannot fault to show it is still written, so a run
			// stays until newer ones push it out. A heap's first generation has no runs yet and records them for a second.
			static constexpr size_t c_HotRunCapacity = 512;
			static constexpr size_t c_HotRunBytes = size_t{32} << 20;
			static constexpr uint64_t c_HotTicks = 4, c_FirstHotTicks = 60;
			std::array<uint32_t, c_HotRunCapacity> hotRuns{};
			size_t hotFirst = 0, hotCount = 0;
			std::vector<uint8_t> hotMarks;
			uint64_t armedTick = 0, hotTicks = c_FirstHotTicks;
			size_t HotCapacity() const noexcept { return std::min(c_HotRunCapacity, std::max<size_t>(1, c_HotRunBytes / (c_FaultRunPages * pageBytes))); }
			void RememberHotRun(size_t run) noexcept {
				if (run >= hotMarks.size() || hotMarks[run]) return;
				if (hotCount == HotCapacity()) {
					hotMarks[hotRuns[hotFirst]] = 0;
					hotFirst = (hotFirst + 1) % c_HotRunCapacity;
					--hotCount;
				}
				hotRuns[(hotFirst + hotCount) % c_HotRunCapacity] = static_cast<uint32_t>(run);
				hotMarks[run] = 1;
				++hotCount;
			}
			// A busy heap with few hot runs yet is copied whole at its boundary and fenced only to learn them: the game's first
			// write to a run faults once, records the run and opens it whole, with nothing left to copy.
			std::vector<uint8_t> learnMarks;
			size_t learnPages = 0;
			bool learning = false;
			void OpenLearnedRuns() noexcept {
				for (size_t run = 0; learning && run < learnMarks.size();) {
					if (!learnMarks[run]) { ++run; continue; }
					const size_t begin = run;
					while (run < learnMarks.size() && learnMarks[run]) learnMarks[run++] = 0;
					const size_t first = begin * c_FaultRunPages, last = std::min(run * c_FaultRunPages, learnPages);
					if (first < last) PageWriteFence::OpenCopiedPage(base + first * pageBytes, (last - first) * pageBytes);
				}
				learning = false;
			}
			size_t HotBytes() noexcept {
				Locked guard(lock);
				return hotCount * c_FaultRunPages * pageBytes;
			}
			bool firstTickMeasured = true;
			std::array<uint32_t, 2> FirstTickWrites() noexcept {
				Locked guard(lock);
				if (firstTickMeasured || SaverThreads::TickStarts() - armedTick < 1) return {};
				firstTickMeasured = true;
				std::array<uint32_t, 2> counts{};
				for (const auto& copy: copies) {
					if (copy->armed.load(std::memory_order_relaxed) != 1 || copy->completed) continue;
					for (size_t page = 0; page < copy->saved.size(); ++page) {
						if (!copy->saved[page]) continue;
						++counts[0];
						counts[1] += std::memcmp(reinterpret_cast<const std::byte*>(copy->destination) + page * pageBytes, reinterpret_cast<const void*>(base + page * pageBytes), pageBytes) != 0;
					}
				}
				return counts;
			}
			size_t SavedPages(const CowCopy& copy) noexcept {
				Locked guard(lock);
				return static_cast<size_t>(std::count(copy.saved.begin(), copy.saved.end(), uint8_t{1}));
			}
			std::atomic<uint32_t> tickFaults{0}, tickCollectFaults{0};
			uint32_t armedHotRuns = 0;
			bool armedWhole = false;
			struct Locked {
				std::atomic_flag& flag;
				explicit Locked(std::atomic_flag& flag) : flag(flag) { while (flag.test_and_set(std::memory_order_acquire)) {} }
				~Locked() { flag.clear(std::memory_order_release); }
			};
			struct Prepared { CowCoordinator* coordinator; std::shared_ptr<CowCopy> copy; };
			void Prepare(const std::shared_ptr<CowCopy>& copy) {
				const size_t runs = (copy->saved.size() + c_FaultRunPages - 1) / c_FaultRunPages;
				Locked guard(lock);
				std::erase_if(copies, [](const auto& value) { return value->completed; });
				copies.push_back(copy);
				if (hotMarks.size() < runs) hotMarks.resize(runs, 0);
				if (learnMarks.size() < runs) learnMarks.resize(runs, 0);
			}
			// A boundary arming on several threads plans under the lock, copies outside it while the copy is unarmed (no fault
			// or drain touches an unarmed copy), then fences what it did not copy and arms under the lock.
			bool PlanArm(const CowCopy& copy, size_t bytes, bool whole, std::vector<std::pair<uint32_t, uint32_t>>& spans) noexcept {
				Locked guard(lock);
				if (copy.completed || copy.armed.load(std::memory_order_relaxed) != 0) return false;
				if (whole && bytes != copy.saved.size() * copy.pageBytes) return false;
				const size_t pages = std::min(copy.saved.size(), bytes / pageBytes);
				spans.clear();
				if (whole) {
					if (pages) spans.emplace_back(0u, static_cast<uint32_t>(pages));
					return true;
				}
				for (size_t index = 0; index < hotCount && spans.size() < spans.capacity(); ++index) {
					const size_t first = size_t{hotRuns[(hotFirst + index) % c_HotRunCapacity]} * c_FaultRunPages;
					const size_t last = std::min(first + c_FaultRunPages, pages);
					for (size_t page = first; page < last && spans.size() < spans.capacity();) {
						if (copy.saved[page]) { ++page; continue; }
						const size_t begin = page;
						while (page < last && !copy.saved[page]) ++page;
						spans.emplace_back(static_cast<uint32_t>(begin), static_cast<uint32_t>(page - begin));
					}
				}
				return true;
			}
			bool FinishArm(CowCopy& copy, uintptr_t address, size_t bytes, bool whole, bool learn, size_t copiedPages, int64_t copyUs) noexcept {
				Locked guard(lock);
				if (copy.completed || copy.armed.load(std::memory_order_relaxed) != 0) return false;
				const size_t pages = std::min(copy.saved.size(), bytes / pageBytes);
				if (whole && learn && !learning && pages) {
					if (!PageWriteFence::ProtectCopyPages(address, pages * pageBytes)) return false;
					std::fill(learnMarks.begin(), learnMarks.begin() + static_cast<std::ptrdiff_t>(std::min(learnMarks.size(), (pages + c_FaultRunPages - 1) / c_FaultRunPages)), uint8_t{1});
					learnPages = pages;
					learning = true;
				}
				for (size_t page = 0; page < pages;) {
					if (copy.saved[page]) { ++page; continue; }
					const size_t begin = page;
					while (page < pages && !copy.saved[page]) ++page;
					if (!PageWriteFence::ProtectCopyPages(address + begin * pageBytes, (page - begin) * pageBytes)) return false;
				}
				if (pages * pageBytes < bytes && !PageWriteFence::ProtectCopyPages(address + pages * pageBytes, bytes - pages * pageBytes)) return false;
				if (copy.faults) {
					copy.faults->armCopyUs = copyUs;
					copy.faults->armCopiedPages = copiedPages;
					copy.faults->armWhole = whole;
				}
				armedHotRuns = whole ? 0 : static_cast<uint32_t>(hotCount);
				armedWhole = whole;
				armedTick = SaverThreads::TickStarts();
				hotTicks = hotCount ? c_HotTicks : c_FirstHotTicks;
				firstTickMeasured = !TallyingFaults();
				copy.armed.store(1, std::memory_order_release);
				copy.armed.notify_all();
				return true;
			}
			static bool Arm(void* context, uintptr_t address, size_t bytes) noexcept {
				auto& prepared = *static_cast<Prepared*>(context);
				auto& coordinator = *prepared.coordinator;
				Locked guard(coordinator.lock);
				auto& copy = *prepared.copy;
				if (copy.completed || copy.armed.load(std::memory_order_relaxed) != 0) return false;
				// The hot runs are copied here instead of fenced: the ticks after a boundary write them again, and their faults
				// cost those ticks several times what copying them costs the boundary.
				const auto copyStarted = RTE::SteadyClock::now();
				const size_t pages = std::min(copy.saved.size(), bytes / coordinator.pageBytes);
				size_t copiedPages = 0;
				for (size_t index = 0; index < coordinator.hotCount; ++index) {
					const size_t first = size_t{coordinator.hotRuns[(coordinator.hotFirst + index) % c_HotRunCapacity]} * c_FaultRunPages;
					const size_t last = std::min(first + c_FaultRunPages, pages);
					for (size_t page = first; page < last;) {
						if (copy.saved[page]) { ++page; continue; }
						const size_t begin = page;
						while (page < last && !copy.saved[page]) copy.saved[page++] = 1;
						CopyCheckpointPages(reinterpret_cast<std::byte*>(copy.destination) + begin * coordinator.pageBytes,
						                    reinterpret_cast<const void*>(address + begin * coordinator.pageBytes), (page - begin) * coordinator.pageBytes);
						copiedPages += page - begin;
					}
				}
				if (copy.faults) {
					copy.faults->armCopyUs = MicrosecondsSince(copyStarted);
					copy.faults->armCopiedPages = copiedPages;
					copy.faults->armWhole = false;
				}
				coordinator.armedHotRuns = static_cast<uint32_t>(coordinator.hotCount);
				coordinator.armedWhole = false;
				coordinator.armedTick = SaverThreads::TickStarts();
				coordinator.hotTicks = coordinator.hotCount ? c_HotTicks : c_FirstHotTicks;
				for (size_t page = 0; page < pages;) {
					if (copy.saved[page]) { ++page; continue; }
					const size_t begin = page;
					while (page < pages && !copy.saved[page]) ++page;
					if (!PageWriteFence::ProtectCopyPages(address + begin * coordinator.pageBytes, (page - begin) * coordinator.pageBytes)) return false;
				}
				if (pages * coordinator.pageBytes < bytes && !PageWriteFence::ProtectCopyPages(address + pages * coordinator.pageBytes, bytes - pages * coordinator.pageBytes)) return false;
				copy.armed.store(1, std::memory_order_release);
				copy.armed.notify_all();
				return true;
			}
			// At a tick boundary no script writes the heap, so a heap about to be written all over is copied whole at once:
			// no page is fenced, so none faults, opens again or takes the reopened page's first-write fault.
			static bool ArmByCopy(void* context, uintptr_t address, size_t bytes) noexcept {
				auto& prepared = *static_cast<Prepared*>(context);
				auto& coordinator = *prepared.coordinator;
				Locked guard(coordinator.lock);
				auto& copy = *prepared.copy;
				if (copy.completed || copy.armed.load(std::memory_order_relaxed) != 0 || bytes != copy.saved.size() * copy.pageBytes) return false;
				const auto copyStarted = RTE::SteadyClock::now();
				CopyCheckpointPages(copy.destination, reinterpret_cast<const void*>(address), bytes);
				std::fill(copy.saved.begin(), copy.saved.end(), 1);
				if (copy.faults) {
					copy.faults->armCopyUs = MicrosecondsSince(copyStarted);
					copy.faults->armCopiedPages = copy.saved.size();
					copy.faults->armWhole = true;
				}
				coordinator.armedHotRuns = 0;
				coordinator.armedWhole = true;
				coordinator.armedTick = SaverThreads::TickStarts();
				coordinator.hotTicks = coordinator.hotCount ? c_HotTicks : c_FirstHotTicks;
				copy.armed.store(1, std::memory_order_release);
				copy.armed.notify_all();
				return true;
			}

			// A write fault saves and opens the aligned run of pages around it: a collector's pass writes the neighbours
			// next, and taking a fault costs several times what copying a page does.
			static constexpr size_t c_FaultRunPages = 16;
			bool CopyPage(size_t page, bool fault) noexcept {
				const auto started = fault ? RTE::SteadyClock::now() : RTE::SteadyClock::time_point{};
				Locked guard(lock);
				size_t first = page, last = page + 1;
				if (fault) {
					// Every page below an armed generation's end is committed, and an unsaved one is still fenced.
					size_t end = 0;
					for (const auto& copy: copies)
						if (copy->armed.load(std::memory_order_relaxed) == 1 && !copy->completed) end = std::max(end, copy->saved.size());
					first = page - page % c_FaultRunPages;
					last = std::max(page + 1, std::min(first + c_FaultRunPages, end));
				}
				// The faulting page always opens; a neighbour opens only when this call saved it, since it is still fenced.
				std::array<bool, c_FaultRunPages> opened{};
				opened[page - first] = fault;
				bool recorded = !fault || t_Collecting || SaverThreads::Current() || SaverThreads::TickStarts() - armedTick > hotTicks;
				for (const auto& copy: copies) {
					if (copy->armed.load(std::memory_order_relaxed) != 1 || copy->completed) continue;
					const bool faulted = page < copy->saved.size() && !copy->saved[page];
					if (faulted && !recorded) {
						RememberHotRun(first / c_FaultRunPages);
						recorded = true;
					}
					// An unsaved generation's page has never been opened for a
					// write. Materialize every such generation before opening it.
					for (size_t at = first; at < std::min(last, copy->saved.size()); ++at) {
						if (copy->saved[at]) continue;
						CopyCheckpointPages(reinterpret_cast<std::byte*>(copy->destination) + at * pageBytes, reinterpret_cast<const void*>(base + at * pageBytes), pageBytes);
						copy->saved[at] = 1;
						opened[at - first] = true;
					}
					if (fault && faulted) {
						copy->faults->count.fetch_add(1, std::memory_order_relaxed);
						copy->faults->us.fetch_add(MicrosecondsSince(started), std::memory_order_relaxed);
					}
				}
				if (fault && learning && first / c_FaultRunPages < learnMarks.size() && learnMarks[first / c_FaultRunPages]) {
					learnMarks[first / c_FaultRunPages] = 0;
					for (size_t at = first; at < std::min(last, learnPages); ++at) opened[at - first] = true;
					if (!recorded) RememberHotRun(first / c_FaultRunPages);
				}
				if (fault && TallyingFaults()) {
					(t_Collecting ? tickCollectFaults : tickFaults).fetch_add(1, std::memory_order_relaxed);
					FaultTally()[0].fetch_add(1, std::memory_order_relaxed);
					FaultTally()[1].fetch_add(MicrosecondsSince(started), std::memory_order_relaxed);
				}
				return OpenRuns(first, opened.data(), last - first);
			}
			// Opening a page that is open already makes the game's next write to it take a fault, so only runs of pages that
			// were still fenced open.
			bool OpenRuns(size_t first, const bool* opened, size_t count) noexcept {
				for (size_t at = 0; at < count;) {
					if (!opened[at]) { ++at; continue; }
					const size_t begin = at;
					while (at < count && opened[at]) ++at;
					if (!PageWriteFence::OpenCopiedPage(base + (first + begin) * pageBytes, (at - begin) * pageBytes)) return false;
				}
				return true;
			}
			// A page opened again after a fence still makes its first write fault while the system marks it written; a saver
			// takes that fault in the game's idle time with a write that changes nothing.
			void TouchPages(size_t first, size_t count) noexcept {
				{
					// A learning heap stays fenced until the game's first write to each run; a touch would end its learning.
					Locked guard(lock);
					if (learning) return;
				}
				for (size_t page = first; page < first + count; ++page) {
#ifdef _WIN32
					InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(base + page * pageBytes), 0);
#else
					__atomic_fetch_add(reinterpret_cast<uint32_t*>(base + page * pageBytes), 0u, __ATOMIC_RELAXED);
#endif
				}
			}
			static constexpr size_t c_CopyRunPages = 64;
			bool CopyPages(size_t first, size_t count) noexcept {
				Locked guard(lock);
				std::array<bool, c_CopyRunPages> opened{};
				const bool runs = count <= opened.size();
				for (const auto& copy: copies) {
					if (copy->armed.load(std::memory_order_relaxed) != 1 || copy->completed) continue;
					const size_t last = std::min(first + count, copy->saved.size());
					for (size_t page = first; page < last;) {
						if (copy->saved[page]) { ++page; continue; }
						const size_t begin = page;
						while (page < last && !copy->saved[page]) ++page;
						CopyCheckpointPages(reinterpret_cast<std::byte*>(copy->destination) + begin * pageBytes,
						                    reinterpret_cast<const void*>(base + begin * pageBytes), (page - begin) * pageBytes);
						std::fill(copy->saved.begin() + begin, copy->saved.begin() + page, 1);
						if (runs) std::fill(opened.begin() + (begin - first), opened.begin() + (page - first), true);
					}
				}
				// Every active generation owns this batch before live writes resume on it.
				return runs ? OpenRuns(first, opened.data(), count) : PageWriteFence::OpenCopiedPage(base + first * pageBytes, count * pageBytes);
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
				// Only the pages a generation never saved are still fenced.
				const auto fenced = [this](size_t page) {
					return std::any_of(copies.begin(), copies.end(), [page](const auto& generation) {
						return generation->armed.load(std::memory_order_relaxed) == 1 && page < generation->saved.size() && !generation->saved[page];
					});
				};
				for (size_t page = 0; page < pages;) {
					if (!fenced(page)) { ++page; continue; }
					const size_t begin = page;
					while (page < pages && fenced(page)) ++page;
					if (!PageWriteFence::OpenCopiedPage(base + begin * pageBytes, (page - begin) * pageBytes)) return false;
				}
				OpenLearnedRuns();
				return true;
			}
			void Cancel(const std::shared_ptr<CowCopy>& copy) {
				Locked guard(lock);
				std::erase(copies, copy);
				if (copy) {
					copy->armed.store(2, std::memory_order_release);
					copy->armed.notify_all();
					copy->completed = true; copy->buffer.reset();
				}
				if (std::all_of(copies.begin(), copies.end(), [](const auto& generation) { return generation->armed.load(std::memory_order_relaxed) != 1 || generation->completed; }) && copy) {
					PageWriteFence::OpenCopiedPage(base, copy->saved.size() * pageBytes);
					std::fill(learnMarks.begin(), learnMarks.end(), uint8_t{0});
					learning = false;
				}
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
		static std::atomic<size_t>& HeapCommitted() {
			static std::atomic<size_t> bytes{0};
			return bytes;
		}
		static std::atomic<size_t>& SlabCommitted() {
			static std::atomic<size_t> bytes{0};
			return bytes;
		}
		static std::atomic<bool>& KeepWarm() {
			static std::atomic<bool> warm{false};
			return warm;
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
		static std::unordered_map<lua_State*, std::weak_ptr<PreparationLife>>& StatePreparations() {
			static auto* states = new std::unordered_map<lua_State*, std::weak_ptr<PreparationLife>>();
			return *states;
		}
		static std::atomic<size_t>& DeadHeapCalls() {
			static std::atomic<size_t> calls{0};
			return calls;
		}
		std::shared_ptr<Slab> TakeSlab(size_t pages, bool bounded = false, bool commit = true) {
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
				slab->committed.store(commit, std::memory_order_relaxed);
				slab->pages = static_cast<Snapshot::Page*>(MapPages(slab->capacity * Snapshot::c_PageBytes, commit));
				if (!slab->pages) throw std::bad_alloc();
				m_FreshBytes += slab->capacity * Snapshot::c_PageBytes;
				CopyBytes(false).fetch_add(slab->capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
				if (commit) SlabCommitted().fetch_add(slab->capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
			} else if (commit && !slab->Commit()) {
				throw std::bad_alloc();
			}
			reservation.transferred = true;
			return std::shared_ptr<Slab>(slab.release(), [](Slab* returned) { ReturnSlab(returned->owner, returned); });
		}
		// A buffer goes back to its heap only while that heap exists; one released after it is unmapped instead. With no
		// capture near it keeps no memory, and a game thread leaves giving it back to the savers.
		static void ReturnSlab(HeapOwner* owner, Slab* slab) {
			if (!KeepWarm().load(std::memory_order_acquire) && slab->committed.load(std::memory_order_acquire)) {
				if (!SaverThreads::Current() && SaverThreads::Gated()) try {
					SaverThreads::Pool().push_task([owner, slab] { ReturnSlab(owner, slab); });
					return;
				} catch (...) {}
				slab->Decommit();
			}
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
			if (slab->committed.load(std::memory_order_acquire)) SlabCommitted().fetch_sub(slab->capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
			slab->pages = nullptr;
			delete slab;
		}

		static int64_t MicrosecondsSince(RTE::SteadyClock::time_point started) {
			return std::chrono::duration_cast<std::chrono::microseconds>(RTE::SteadyClock::now() - started).count();
		}

		// Copies every committed page; nothing writes the heap meanwhile. A capture's collection marks every live object, so
		// nearly every page is written between two freezes: keeping the last copy to share the rest would hold a second
		// copy of the heap for a tenth of the copying, and the buffer it pins could never be reused.
		void CopyPages(Snapshot::Data& data) {
			const auto copyStarted = RTE::SteadyClock::now();
			const size_t pageCount = data.committed / Snapshot::c_PageBytes;
			if (pageCount != 0) {
				std::shared_ptr<Slab> slab = TakeSlab(pageCount);
				CopyCheckpointPages(slab->pages, reinterpret_cast<const void*>(m_Base), pageCount * Snapshot::c_PageBytes);
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
			if (!m_Bootstrap) throw std::bad_alloc();
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
			if (!m_State) throw std::bad_alloc();
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

		// A saver's worker gives its pages back a piece at a time between the game's ticks: one large release holds the
		// process's address space while the game's own page faults wait on it.
		template<class Give> void ReleaseCommitted(Give give) noexcept {
			const size_t piece = m_YieldsToGame ? size_t{1} << 20 : m_Committed;
			for (size_t offset = 0; offset < m_Committed; offset += piece) {
				if (offset) try { SaverThreads::YieldToGame(); } catch (...) {}
				give(m_Base + offset, std::min(piece, m_Committed - offset));
			}
		}

#ifdef _WIN32
		static void* MapPages(size_t bytes, bool commit = true) noexcept {
			return VirtualAlloc(nullptr, bytes, commit ? MEM_RESERVE | MEM_COMMIT : MEM_RESERVE, commit ? PAGE_READWRITE : PAGE_NOACCESS);
		}
		static bool CommitPages(void* pages, size_t bytes) noexcept { return VirtualAlloc(pages, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr; }
		static void DecommitPages(void* pages, size_t bytes) noexcept { VirtualFree(pages, bytes, MEM_DECOMMIT); }
		static void Unmap(void* pages, size_t) noexcept { VirtualFree(pages, 0, MEM_RELEASE); }
		void Reserve(size_t shift) {
			void* base = VirtualAlloc(nullptr, c_ReserveBytes + c_Colors * c_PageStride, MEM_RESERVE, PAGE_NOACCESS);
			if (!base) throw std::runtime_error("could not reserve the tracked Lua heap");
			m_HeapMemory = std::shared_ptr<void>(base, [](void* value) { Unmap(value, c_ReserveBytes + c_Colors * c_PageStride); });
			m_Reservation = reinterpret_cast<uintptr_t>(base);
			m_Base = m_Reservation + shift;
		}
		bool Commit(size_t bytes) noexcept {
			return VirtualAlloc(reinterpret_cast<void*>(m_Base + m_Committed), bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
		}
		void Release() noexcept {
			// Saved addresses stay distinct after closure without retaining the VM's committed pages.
			if (m_Reservation && m_Committed) ReleaseCommitted([](uintptr_t at, size_t bytes) { VirtualFree(reinterpret_cast<void*>(at), bytes, MEM_DECOMMIT); });
			m_HeapMemory.reset();
			m_Reservation = m_Base = 0;
		}
#else
		// An anonymous mapping takes memory only as it is touched, so a reserved buffer is mapped the same way.
		static void* MapPages(size_t bytes, bool = true) noexcept {
			void* pages = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			return pages == MAP_FAILED ? nullptr : pages;
		}
		static bool CommitPages(void*, size_t) noexcept { return true; }
		static void DecommitPages(void* pages, size_t bytes) noexcept { madvise(pages, bytes, MADV_DONTNEED); }
		static void Unmap(void* pages, size_t bytes) noexcept { munmap(pages, bytes); }
		void Reserve(size_t shift) {
			void* base = mmap(nullptr, c_ReserveBytes + c_Colors * c_PageStride, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
			if (base == MAP_FAILED) throw std::runtime_error("could not reserve the tracked Lua heap");
			m_HeapMemory = std::shared_ptr<void>(base, [](void* value) { Unmap(value, c_ReserveBytes + c_Colors * c_PageStride); });
			m_Reservation = reinterpret_cast<uintptr_t>(base);
			m_Base = m_Reservation + shift;
		}
		bool Commit(size_t bytes) noexcept {
			return mprotect(reinterpret_cast<void*>(m_Base + m_Committed), bytes, PROT_READ | PROT_WRITE) == 0;
		}
		void Release() noexcept {
			if (m_Reservation && m_Committed) ReleaseCommitted([](uintptr_t at, size_t bytes) {
				mprotect(reinterpret_cast<void*>(at), bytes, PROT_NONE);
				madvise(reinterpret_cast<void*>(at), bytes, MADV_DONTNEED);
			});
			m_HeapMemory.reset();
			m_Reservation = m_Base = 0;
		}
#endif

		void* Bump(size_t bytes) noexcept {
			if (bytes > c_ReserveBytes - m_Used) return nullptr;
			if (m_Used + bytes > m_Committed) {
				const size_t needed = m_Used + bytes - m_Committed;
				const size_t step = std::max(c_CommitStep, (needed + c_CommitStep - 1) / c_CommitStep * c_CommitStep);
				if (m_Committed + step > c_ReserveBytes || !Commit(step)) return nullptr;
				if (m_PrivateNative) {
					void* begin = reinterpret_cast<void*>(m_Base + m_Committed);
					bool recorded = false;
					try {
						CheckpointNativeStorage::RememberPrivate(begin, step); recorded = true;
						m_PrivateChunks.emplace_back(reinterpret_cast<uintptr_t>(begin), step);
					} catch (...) {
						if (recorded) CheckpointNativeStorage::ForgetPrivate(begin);
						return nullptr;
					}
				}
				m_Committed += step;
				HeapCommitted().fetch_add(step, std::memory_order_relaxed);
				m_PublishedCommitted.store(m_Committed, std::memory_order_release);
				if (m_PrivateRead) m_PrivateRead->committed.store(m_Committed, std::memory_order_release);
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
			if (owner.m_YieldsToGame) SaverThreads::YieldToGame();
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

	/// One prepared heap in a boundary that arms on several threads: Arm's locks, held from the plan to the fence, and the
	/// page spans the boundary copies.
	class HeapOwner::ArmSession {
	public:
		ArmSession() = default;
		ArmSession(ArmSession&&) noexcept = default;
		ArmSession& operator=(ArmSession&&) noexcept = default;
		const std::vector<std::pair<uint32_t, uint32_t>>& Spans() const noexcept { return m_Spans; }
		void CopyPages(size_t first, size_t count) const noexcept;
		/// Fences what the plan did not copy and publishes the snapshot.
		bool Finish(size_t copiedPages, int64_t copyUs) noexcept;
	private:
		friend class HeapOwner;
		friend class HeapOwner::Prepared;
		friend class HeapOwner::PreparationLease;
		std::unique_lock<std::mutex> m_Alive, m_Preparations, m_Lock;
		Prepared* m_Prepared = nullptr;
		HeapOwner* m_Owner = nullptr;
		bool m_Whole = false, m_Learn = false;
		RTE::SteadyClock::time_point m_Started{};
		std::vector<std::pair<uint32_t, uint32_t>> m_Spans;
	};

	class HeapOwner::Prepared {
	public:
		~Prepared() { Cancel(); }
		Prepared(const Prepared&) = delete;
		Prepared& operator=(const Prepared&) = delete;
		bool Current(const HeapOwner& owner) const noexcept {
			return m_Data->state == owner.m_State && m_Data->committed == owner.m_PublishedCommitted.load(std::memory_order_acquire);
		}
		bool Arm(HeapOwner& owner, bool byCopy = false) noexcept {
			std::unique_lock preparations(owner.m_PrepareMutex, std::try_to_lock);
			if (!preparations || !Current(owner) || owner.m_CowCoordinator != m_Coordinator) return false;
			if (owner.m_CopyPending.load(std::memory_order_acquire) && !owner.m_CowGateFree.load(std::memory_order_acquire)) return false;
			std::unique_lock lock(m_Mutex, std::try_to_lock);
			if (!lock || m_Finished || m_Copy->armed.load(std::memory_order_relaxed) != 0) return false;
			const auto started = RTE::SteadyClock::now();
			if (!BeginWrites()) return false;
			m_Data->serial = G(owner.m_State)->objserial;
			CowCoordinator::Prepared prepared{m_Coordinator.get(), m_Copy};
			if (!(byCopy ? CowCoordinator::ArmByCopy : CowCoordinator::Arm)(&prepared, m_Data->base, m_Data->committed)) return false;
			owner.m_CowGateFree.store(true, std::memory_order_release);
			m_Data->freezeUs = MicrosecondsSince(started);
			return true;
		}
		Snapshot Image() const { return Snapshot(m_Data); }
		/// Commits the copy buffer and makes it resident, so a boundary's copies and the faults after it write resident pages;
		/// a run of pages at a time, and none once a boundary has begun.
		void Warm() noexcept {
			constexpr size_t runPages = 64;
			const size_t pageBytes = m_Copy->pageBytes, pages = m_Data->committed / pageBytes;
			auto* destination = reinterpret_cast<volatile std::byte*>(m_Copy->destination);
			for (size_t first = 0; first < pages; first += runPages) {
				m_Touching.store(true, std::memory_order_seq_cst);
				const bool stop = m_WritesBegun.load(std::memory_order_seq_cst) || !m_Buffer->Commit();
				if (!stop) for (size_t page = first; page < std::min(first + runPages, pages); ++page) destination[page * pageBytes] = std::byte{};
				m_Touching.store(false, std::memory_order_seq_cst);
				if (stop) return;
				try { SaverThreads::YieldToGame(); } catch (...) {}
			}
		}
		/// Gives the copy buffer's memory back while no boundary has begun; the boundary commits it again.
		void Cool() noexcept {
			m_Touching.store(true, std::memory_order_seq_cst);
			if (!m_WritesBegun.load(std::memory_order_seq_cst)) m_Buffer->Decommit();
			m_Touching.store(false, std::memory_order_seq_cst);
		}
		/// The bytes still fenced: what the boundary neither copied nor any fault has saved since.
		size_t FencedBytes() const noexcept {
			return m_Data->committed - std::min(m_Data->committed, m_Coordinator->SavedPages(*m_Copy) * m_Copy->pageBytes);
		}
		/// Copies every page still fenced now, on several saver threads beside the game, then the drain finds it saved.
		void CopyFencedNow() {
			std::lock_guard lock(m_Mutex);
			if (m_Finished || m_Copy->armed.load(std::memory_order_acquire) != 1) return;
			static constexpr size_t batchPages = 64;
			const size_t pages = m_Copy->saved.size(), batches = (pages + batchPages - 1) / batchPages;
			const SaverThreads::UngatedScope ungated;
			ParallelWork copies(SaverThreads::Pool(), batches, [this, pages](size_t batch) {
				const SaverThreads::UngatedScope helperUngated;
				const size_t first = batch * batchPages, count = std::min(batchPages, pages - first);
				if (m_Coordinator->CopyPages(first, count)) m_Coordinator->TouchPages(first, count);
			}, 3);
			copies.Finish();
		}
		/// Arm's locks and checks for a boundary arming on several threads, held in the session until it is fenced.
		bool BeginArm(HeapOwner& owner, bool whole, bool learn, ArmSession& session);
		/// The bytes of the heap's hot runs.
		size_t HotBytes() const noexcept { return m_Coordinator->HotBytes(); }
		/// Copies pages of the session's plan into the snapshot; disjoint pages may be copied on different threads at once.
		void CopyArmPages(size_t first, size_t count) const noexcept {
			const size_t pageBytes = m_Copy->pageBytes;
			CopyCheckpointPages(reinterpret_cast<std::byte*>(m_Copy->destination) + first * pageBytes, reinterpret_cast<const void*>(m_Data->base + first * pageBytes),
			                    count * pageBytes);
			std::fill(m_Copy->saved.begin() + static_cast<std::ptrdiff_t>(first), m_Copy->saved.begin() + static_cast<std::ptrdiff_t>(first + count), uint8_t{1});
		}
		bool FinishArm(HeapOwner& owner, bool whole, bool learn, size_t copiedPages, int64_t copyUs, RTE::SteadyClock::time_point started) noexcept {
			if (!m_Coordinator->FinishArm(*m_Copy, m_Data->base, m_Data->committed, whole, learn, copiedPages, copyUs)) return false;
			owner.m_CowGateFree.store(true, std::memory_order_release);
			m_Data->freezeUs = MicrosecondsSince(started);
			return true;
		}
		void Drain() {
			std::unique_lock lock(m_Mutex);
			if (m_Finished) { m_Data->WaitCopied(); return; }
			if (m_Copy->armed.load(std::memory_order_acquire) != 1) throw std::logic_error("Lua heap copy requested before its prepared boundary");
			bool cancelled = false;
			try {
				const auto started = RTE::SteadyClock::now();
				constexpr size_t batchPages = 64;
				for (size_t page = 0; page < m_Copy->saved.size(); page += batchPages) {
					if (!m_Coordinator->CopyPages(page, std::min(batchPages, m_Copy->saved.size() - page)))
						throw std::runtime_error("could not open prepared Lua heap pages");
					m_Coordinator->TouchPages(page, std::min(batchPages, m_Copy->saved.size() - page));
					// The copy stops for the game's ticks between batches; a cancel meanwhile ends it as a cancel before it would.
					{
						lock.unlock();
						struct Relock { std::unique_lock<std::mutex>& lock; ~Relock() { lock.lock(); } } relock{lock};
						SaverThreads::YieldToGame();
					}
					if (m_Finished) { cancelled = true; break; }
				}
				if (!cancelled) {
					if (!m_Coordinator->Complete(m_Copy, true)) throw std::runtime_error("could not open a prepared Lua heap");
					m_Data->copied.store(m_Data->committed / Snapshot::c_PageBytes, std::memory_order_relaxed);
					m_Data->copyUs.store(MicrosecondsSince(started), std::memory_order_relaxed);
					m_Ready.set_value();
					m_Finished = true;
				}
			} catch (...) {
				m_Coordinator->Cancel(m_Copy);
				m_Ready.set_exception(std::current_exception());
				m_Finished = true;
				throw;
			}
			if (cancelled) m_Data->WaitCopied();
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
		Prepared(std::shared_ptr<Snapshot::Data> data, std::shared_ptr<CowCopy> copy, std::shared_ptr<CowCoordinator> coordinator, std::shared_ptr<Slab> buffer) :
		    m_Data(std::move(data)), m_Copy(std::move(copy)), m_Coordinator(std::move(coordinator)), m_Buffer(std::move(buffer)),
		    m_Cancelled(std::make_exception_ptr(std::runtime_error("prepared Lua heap capture was cancelled"))) {
			m_Data->ready = m_Ready.get_future().share();
		}
		// A boundary's writes into the buffer begin here: warming and cooling stop, the run in progress ends, the buffer commits.
		bool BeginWrites() noexcept {
			m_WritesBegun.store(true, std::memory_order_seq_cst);
			while (m_Touching.load(std::memory_order_seq_cst)) std::this_thread::yield();
			return m_Buffer->Commit();
		}
		std::shared_ptr<Snapshot::Data> m_Data;
		std::shared_ptr<CowCopy> m_Copy;
		std::shared_ptr<CowCoordinator> m_Coordinator;
		std::shared_ptr<Slab> m_Buffer;
		std::atomic<bool> m_WritesBegun{false}, m_Touching{false};
		std::promise<void> m_Ready;
		std::exception_ptr m_Cancelled;
		std::mutex m_Mutex;
		bool m_Finished = false;
	};

	inline std::shared_ptr<HeapOwner::Prepared> HeapOwner::PreparationLease::Prepare() const {
		if (!m_Life) throw std::runtime_error("the queued Lua heap preparation has no owner");
		std::lock_guard alive(m_Life->mutex);
		if (!m_Life->owner) throw std::runtime_error("the Lua heap closed before its queued preparation");
		return m_Life->owner->PrepareFreeze();
	}

	inline bool HeapOwner::PreparationLease::Current(const Prepared& prepared) const noexcept {
		if (!m_Life) return false;
		std::unique_lock alive(m_Life->mutex, std::try_to_lock);
		return alive && m_Life->owner && prepared.Current(*m_Life->owner);
	}

	inline bool HeapOwner::PreparationLease::Arm(Prepared& prepared, bool byCopy) const noexcept {
		if (!m_Life) return false;
		std::unique_lock alive(m_Life->mutex, std::try_to_lock);
		return alive && m_Life->owner && prepared.Arm(*m_Life->owner, byCopy);
	}

	inline std::array<uint32_t, 5> HeapOwner::TakeTickFaults() noexcept {
		std::unique_lock preparations(m_PrepareMutex, std::try_to_lock);
		if (!preparations || !m_CowCoordinator) return {};
		auto& coordinator = *m_CowCoordinator;
		CowCoordinator::Locked guard(coordinator.lock);
		return {coordinator.tickFaults.exchange(0, std::memory_order_relaxed), coordinator.tickCollectFaults.exchange(0, std::memory_order_relaxed),
		        coordinator.armedHotRuns, static_cast<uint32_t>(coordinator.hotCount), coordinator.armedWhole ? 1u : 0u};
	}

	inline std::array<uint32_t, 2> HeapOwner::TakeFirstTickWrites() noexcept {
		std::unique_lock preparations(m_PrepareMutex, std::try_to_lock);
		if (!preparations || !m_CowCoordinator) return {};
		return m_CowCoordinator->FirstTickWrites();
	}

	inline bool HeapOwner::PreparationLease::BeginArm(Prepared& prepared, bool whole, bool learn, ArmSession& session) const {
		if (!m_Life) return false;
		session.m_Alive = std::unique_lock(m_Life->mutex, std::try_to_lock);
		return session.m_Alive && m_Life->owner && prepared.BeginArm(*m_Life->owner, whole, learn, session);
	}

	inline bool HeapOwner::Prepared::BeginArm(HeapOwner& owner, bool whole, bool learn, ArmSession& session) {
		session.m_Preparations = std::unique_lock(owner.m_PrepareMutex, std::try_to_lock);
		if (!session.m_Preparations || !Current(owner) || owner.m_CowCoordinator != m_Coordinator) return false;
		if (owner.m_CopyPending.load(std::memory_order_acquire) && !owner.m_CowGateFree.load(std::memory_order_acquire)) return false;
		session.m_Lock = std::unique_lock(m_Mutex, std::try_to_lock);
		if (!session.m_Lock || m_Finished || m_Copy->armed.load(std::memory_order_relaxed) != 0) return false;
		session.m_Started = RTE::SteadyClock::now();
		m_Data->serial = G(owner.m_State)->objserial;
		session.m_Spans.reserve(CowCoordinator::c_HotRunCapacity + 1);
		if (!m_Coordinator->PlanArm(*m_Copy, m_Data->committed, whole, session.m_Spans) || !BeginWrites()) return false;
		session.m_Prepared = this;
		session.m_Owner = &owner;
		session.m_Whole = whole;
		session.m_Learn = whole && learn;
		return true;
	}

	inline void HeapOwner::ArmSession::CopyPages(size_t first, size_t count) const noexcept { m_Prepared->CopyArmPages(first, count); }

	inline bool HeapOwner::ArmSession::Finish(size_t copiedPages, int64_t copyUs) noexcept {
		return m_Prepared && m_Prepared->FinishArm(*m_Owner, m_Whole, m_Learn, copiedPages, copyUs, m_Started);
	}

	inline std::shared_ptr<HeapOwner::Prepared> HeapOwner::PrepareFreeze() {
		std::lock_guard preparations(m_PrepareMutex);
		auto data = std::make_shared<Snapshot::Data>();
		data->state = m_State; data->handlers = m_LibraryHandlers; data->base = m_Base;
		data->sourceMemory = m_HeapMemory;
		data->committed = m_PublishedCommitted.load(std::memory_order_acquire);
		data->periodAllocated = m_PeriodAllocated.load(std::memory_order_relaxed);
		data->busy = m_Busy.load(std::memory_order_relaxed);
		const size_t pageBytes = PageWriteFence::SystemPageBytes();
		if (!data->state || !pageBytes || !data->committed || data->base % pageBytes || data->committed % pageBytes)
			throw std::runtime_error("the prepared Lua heap has no aligned committed range");
		// Reserved only: a capture coming near commits and touches it (Prepared::Warm), the boundary commits it at the latest.
		auto slab = TakeSlab(data->committed / Snapshot::c_PageBytes, true, false);
		auto buffer = slab;
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
		auto prepared = std::shared_ptr<Prepared>(new Prepared(std::move(data), std::move(copy), m_CowCoordinator, std::move(buffer)));
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
		if (committed.load(std::memory_order_acquire)) HeapOwner::SlabCommitted().fetch_sub(capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
	}

	inline bool HeapOwner::Slab::Commit() noexcept {
		if (committed.load(std::memory_order_acquire)) return true;
		if (!HeapOwner::CommitPages(pages, capacity * Snapshot::c_PageBytes)) return false;
		if (!committed.exchange(true, std::memory_order_acq_rel)) HeapOwner::SlabCommitted().fetch_add(capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
		return true;
	}

	inline void HeapOwner::Slab::Decommit() noexcept {
		if (!committed.load(std::memory_order_acquire)) return;
		HeapOwner::DecommitPages(pages, capacity * Snapshot::c_PageBytes);
		committed.store(false, std::memory_order_release);
		HeapOwner::SlabCommitted().fetch_sub(capacity * Snapshot::c_PageBytes, std::memory_order_relaxed);
	}

	inline bool HeapOwner::RunPrivateNativeSelfTest() {
		const std::array<std::shared_ptr<const CheckpointPagePool::Snapshot>, 0> pages{};
		CheckpointNativeStorage::ReadScope reads(pages);
		bool refused = false;
		{
			CheckpointFailure::Scope failure(CheckpointFailure::Point::NativeObjects);
			try { Create(); } catch (const std::bad_alloc&) { refused = failure.Triggered(); }
		}
		if (!refused) return false;
		const auto generation = CheckpointPagePool::InventoryGeneration();
		auto owner = Create();
		struct Value { int n; };
		Value* value = nullptr;
		ProtectedCall(owner->State(), [&] {
			value = static_cast<Value*>(lua_newuserdata(owner->State(), sizeof(Value))); value->n = 37;
			lua_newuserdata(owner->State(), c_CommitStep + 12);
		});
		if (!CheckpointNativeStorage::Private(value, sizeof(Value)) || CheckpointNativeStorage::Source(value) != value ||
		    CheckpointNativeStorage::Source(value)->n != 37 || owner->PrivateNativeRange().bytes <= c_CommitStep) return false;
		owner.reset();
		return !CheckpointNativeStorage::Private(value) && CheckpointPagePool::InventoryGeneration() == generation;
	}

	inline bool HeapOwner::RunBatchOpenSelfTest() {
		const size_t page = PageWriteFence::SystemPageBytes(), run = CowCoordinator::c_FaultRunPages, pages = run * 2, bytes = page * pages;
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
			copy->saved.assign(pages, 0); copy->faults = std::make_shared<CopyFaultStats>();
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
		// The second generation copies the run the first one faulted on at its boundary; the other run stays fenced.
		const auto second = prepare(secondBytes.get());
		if (!second || !coordinator.CopyPages(0, 1)) return false;
		source[0] = 9;
		source[page] = 13;
		if (observer.faults != 1) return false;
		source[run * page] = 17;
		if (observer.faults != 2 || !coordinator.Complete(first, true) || !coordinator.Complete(second, true)) return false;
		const auto* old = static_cast<const unsigned char*>(firstBytes.get());
		const auto* next = static_cast<const unsigned char*>(secondBytes.get());
		return old[0] == 3 && old[page] == 3 && old[run * page] == 3 && next[0] == 5 && next[page] == 7 && next[run * page] == 3 &&
		       source[0] == 9 && source[page] == 13 && source[run * page] == 17;
	}

	inline bool HeapOwner::RunLearningArmSelfTest() {
		const size_t page = PageWriteFence::SystemPageBytes(), run = CowCoordinator::c_FaultRunPages, pages = run * 3, bytes = page * pages;
		if (!page) return false;
		const auto release = [bytes](void* memory) { if (memory) Unmap(memory, bytes); };
		std::unique_ptr<void, decltype(release)> live(MapPages(bytes), release), image(MapPages(bytes), release);
		if (!live || !image) throw std::bad_alloc();
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
		auto copy = std::make_shared<CowCopy>();
		copy->base = coordinator.base; copy->pageBytes = page; copy->destination = static_cast<Snapshot::Page*>(image.get());
		copy->saved.assign(pages, 0); copy->faults = std::make_shared<CopyFaultStats>();
		coordinator.Prepare(copy);
		const auto keepWritable = [](void*, uintptr_t, size_t) noexcept { return true; };
		if (!PageWriteFence::WatchCopies(&observer, {source, bytes}, observe, &observer, keepWritable)) return false;
		std::vector<std::pair<uint32_t, uint32_t>> spans;
		spans.reserve(CowCoordinator::c_HotRunCapacity + 1);
		if (!coordinator.PlanArm(*copy, bytes, true, spans) || spans.size() != 1 || spans[0].second != pages) return false;
		CopyCheckpointPages(image.get(), source, bytes);
		std::fill(copy->saved.begin(), copy->saved.end(), uint8_t{1});
		if (!coordinator.FinishArm(*copy, coordinator.base, bytes, true, true, pages, 0)) return false;
		// The saver's touch leaves the learning heap fenced; the game's first write to each run faults once and opens it whole.
		coordinator.TouchPages(0, pages);
		if (observer.faults) return false;
		source[page] = 7;
		source[2 * page] = 8;
		if (observer.faults != 1) return false;
		source[run * page] = 9;
		if (observer.faults != 2 || coordinator.hotCount != 2) return false;
		// Completing the generation opens the run nobody wrote.
		if (!coordinator.Complete(copy, true)) return false;
		source[2 * run * page] = 10;
		const auto* saved = static_cast<const unsigned char*>(image.get());
		return observer.faults == 2 && saved[page] == 3 && saved[2 * page] == 3 && saved[run * page] == 3 && saved[2 * run * page] == 3 &&
		       source[page] == 7 && source[2 * page] == 8 && source[run * page] == 9 && source[2 * run * page] == 10;
	}

	inline bool HeapOwner::RunPreparationLifetimeSelfTest() {
		auto owner = Create();
		lua_pushnumber(owner->State(), 29);
		const TValue* value = owner->State()->top - 1;
		const auto lease = LeasePreparation(owner->State());
		if (!lease) return false;
		std::shared_ptr<Prepared> prepared;
		{
			CheckpointFailure::Scope failure(CheckpointFailure::Point::LuaPages);
			bool refused = false;
			try { lease.Prepare(); } catch (const std::bad_alloc&) { refused = failure.Triggered(); }
			if (!refused || value->n != 29) return false;
		}
		prepared = std::async(std::launch::async, [&lease] { return lease.Prepare(); }).get();
		if (!lease.Current(*prepared) || !lease.Arm(*prepared)) return false;
		const Snapshot frozen = prepared->Image();
		owner.reset();
		if (lease.Current(*prepared) || lease.Arm(*prepared) || frozen.Read(value).n != 29) return false;
		bool refused = false;
		try { lease.Prepare(); } catch (const std::runtime_error&) { refused = true; }
		if (!refused) return false;
		owner = Create();
		const auto replacement = LeasePreparation(owner->State());
		auto next = replacement.Prepare();
		return replacement.Current(*next) && !lease.Current(*next) && !lease.Arm(*next);
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
