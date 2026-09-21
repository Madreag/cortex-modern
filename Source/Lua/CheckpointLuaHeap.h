#pragma once

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_vmevent.h"
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
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

	// The VM's memory at one freeze. On Windows the live pages are made read-only at the freeze and
	// copied by whoever touches one first: the VM's thread at its first write (from the fault), or the
	// writer at its first read; an untouched page is read live, unchanged since the freeze. Elsewhere
	// every committed page is copied at the freeze. Addresses identify the source VM.
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
		int64_t CopyUs() const { return m_Data ? m_Data->copyUs : 0; }
		size_t FaultCount() const { return m_Data ? m_Data->faults.load(std::memory_order_relaxed) : 0; }

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
		// The copies of one freeze: every page copied for it comes from here, so a freeze allocates once.
		struct Slab {
			Page* pages = nullptr;
			size_t capacity = 0;
			std::atomic<size_t> next{0};
			~Slab();
			Page* Take() {
				const size_t index = next.fetch_add(1, std::memory_order_relaxed);
				return index < capacity ? pages + index : nullptr;
			}
		};

		struct Data {
			lua_State* state = nullptr;
			uint64_t serial = 0;
			uintptr_t base = 0;
			size_t committed = 0;
			size_t pageCount = 0;
			int64_t freezeUs = 0;
			int64_t copyUs = 0;
			HeapOwner* owner = nullptr;
			std::unique_ptr<Slab> slab;
			std::unique_ptr<std::atomic<const Page*>[]> slots; // One per committed page; empty means read live.
			mutable std::atomic<size_t> copied{0};
			mutable std::atomic<size_t> faults{0};
			mutable std::atomic<bool> exhausted{false};
			mutable std::deque<std::vector<std::byte>> assembled; // Read by the one worker that walks this snapshot.
			~Data();

			// The copy a page has, or one made now from its unchanged live bytes.
			const Page* Materialize(size_t index) const {
				if (const Page* page = slots[index].load(std::memory_order_acquire)) return page;
				Page* copy = slab->Take();
				if (!copy) { exhausted.store(true, std::memory_order_relaxed); return nullptr; }
				std::memcpy(copy->bytes, reinterpret_cast<const void*>(base + index * c_PageBytes), c_PageBytes);
				// The fault handler publishes before it lets the write through: whoever is first has the pristine page.
				const Page* expected = nullptr;
				if (slots[index].compare_exchange_strong(expected, copy, std::memory_order_acq_rel)) {
					copied.fetch_add(1, std::memory_order_relaxed);
					return copy;
				}
				return expected;
			}
		};
		std::shared_ptr<const Data> m_Data;
		explicit Snapshot(std::shared_ptr<const Data> data) : m_Data(std::move(data)) {}

		const std::byte* PageBytes(size_t index) const {
			const Page* page = m_Data->Materialize(index);
			if (!page) throw std::runtime_error("a Lua heap snapshot ran out of copy pages");
			return page->bytes;
		}

		friend class HeapOwner;
	};

	// Owns the VM and the reservation every block comes from, and keeps the snapshots taken of it
	// consistent: a write to a page a live snapshot still reads live is caught first and the page copied.
	class HeapOwner {
	public:
		static std::unique_ptr<HeapOwner> Create() {
			auto owner = std::unique_ptr<HeapOwner>(new HeapOwner());
			owner->Initialize();
			return owner;
		}

		~HeapOwner() {
			// The wrapper keeps lua_close from destroying the bootstrap's shared arena.
			if (m_State) lua_close(std::exchange(m_State, nullptr));
			m_Bootstrap.reset();
			// A writer still walking a snapshot reads the live pages; the arena outlives it.
			while (m_LiveCount.load(std::memory_order_acquire) > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			Registry::Remove(this);
			Release();
		}
		HeapOwner(const HeapOwner&) = delete;
		HeapOwner& operator=(const HeapOwner&) = delete;
		HeapOwner(HeapOwner&&) = delete;
		HeapOwner& operator=(HeapOwner&&) = delete;

		lua_State* State() const { return m_State; }

		AllocationStats Stats() const { return {m_Bytes, m_Blocks}; }

		// The caller must hold the VM's execution lock throughout: nothing may write while the pages are protected.
		Snapshot Freeze() {
			const auto started = std::chrono::steady_clock::now();
			if (!m_State) throw std::runtime_error("a Lua heap capture has no state");
			void* allocatorData = nullptr;
			if (lua_getallocf(m_State, &allocatorData) != &Allocate || allocatorData != this)
				throw std::runtime_error("the Lua heap allocator changed after tracking began");
			const size_t committed = m_Committed.load(std::memory_order_relaxed);
			const size_t pageCount = committed / Snapshot::c_PageBytes;
			auto data = std::make_shared<Snapshot::Data>();
			data->state = m_State;
			data->serial = G(m_State)->objserial;
			data->base = m_Base;
			data->committed = committed;
			data->pageCount = pageCount;
			data->owner = this;
			// Every page may be copied once by the fault and once by the writer before either wins.
			data->slab = TakeSlab(2 * pageCount);
			data->slots.reset(new std::atomic<const Snapshot::Page*>[pageCount]);
			for (size_t index = 0; index < pageCount; ++index) data->slots[index].store(nullptr, std::memory_order_relaxed);
			const auto copyStarted = std::chrono::steady_clock::now();
			Publish(data.get());
			if (!Protect(committed)) {
				Unpublish(data.get());
				throw std::runtime_error("the tracked Lua heap could not be protected");
			}
			data->copyUs = MicrosecondsSince(copyStarted);
			data->freezeUs = MicrosecondsSince(started);
			return Snapshot(std::move(data));
		}

	private:
		static constexpr size_t c_ReserveBytes = size_t(1) << 32; // Address space only; committed as the VM grows.
		static constexpr size_t c_CommitStep = size_t(1) << 20;
		static constexpr size_t c_LargeLimit = size_t(1) << 18; // Above this a block takes whole pages of its own.
		static constexpr size_t c_ClassCount = 64 + 56 + 62;
		static constexpr size_t c_LiveLimit = 16;

		// The snapshots a fault may still have to copy for, published whole so a fault reads without a lock.
		struct LiveList {
			Snapshot::Data* items[c_LiveLimit] = {};
			size_t count = 0;
		};

		// Every tracked reservation, so a fault anywhere in the process finds its owner.
		struct Registry {
			static std::atomic<std::shared_ptr<const std::vector<HeapOwner*>>>& Owners() {
				static std::atomic<std::shared_ptr<const std::vector<HeapOwner*>>> owners{std::make_shared<const std::vector<HeapOwner*>>()};
				return owners;
			}
			static std::mutex& Mutex() {
				static std::mutex mutex;
				return mutex;
			}
			static void Add(HeapOwner* owner) {
				std::lock_guard lock(Mutex());
				auto next = std::make_shared<std::vector<HeapOwner*>>(*Owners().load());
				next->push_back(owner);
				Owners().store(std::move(next));
			}
			static void Remove(HeapOwner* owner) {
				std::lock_guard lock(Mutex());
				auto next = std::make_shared<std::vector<HeapOwner*>>(*Owners().load());
				std::erase(*next, owner);
				Owners().store(std::move(next));
			}
			static HeapOwner* Find(uintptr_t address) {
				const auto owners = Owners().load();
				for (HeapOwner* owner: *owners) {
					if (address >= owner->m_Base && address - owner->m_Base < owner->m_Committed.load(std::memory_order_acquire)) return owner;
				}
				return nullptr;
			}
		};

		HeapOwner() = default;
		std::unique_ptr<lua_State, decltype(&lua_close)> m_Bootstrap{nullptr, lua_close};
		lua_State* m_State = nullptr;
		uintptr_t m_Base = 0;
		std::atomic<size_t> m_Committed{0};
		size_t m_Used = 0;
		size_t m_Bytes = 0;
		size_t m_Blocks = 0;
		void* m_Free[c_ClassCount] = {};
		std::vector<std::pair<void*, size_t>> m_LargeFree; // Whole-page blocks given back, by their rounded size.
		std::atomic<std::shared_ptr<const LiveList>> m_Live{std::make_shared<const LiveList>()};
		std::atomic<size_t> m_LiveCount{0};
		std::mutex m_SlabMutex;
		std::vector<std::unique_ptr<Snapshot::Slab>> m_IdleSlabs;

		static int64_t MicrosecondsSince(std::chrono::steady_clock::time_point started) {
			return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
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

		void Publish(Snapshot::Data* data) {
			std::lock_guard lock(m_SlabMutex);
			auto next = std::make_shared<LiveList>(*m_Live.load());
			if (next->count >= c_LiveLimit) throw std::runtime_error("too many live Lua heap snapshots");
			next->items[next->count++] = data;
			m_LiveCount.fetch_add(1, std::memory_order_acq_rel);
			m_Live.store(std::move(next));
		}
		void Unpublish(Snapshot::Data* data) {
			std::lock_guard lock(m_SlabMutex);
			auto next = std::make_shared<LiveList>();
			for (const auto current = m_Live.load(); Snapshot::Data* item: std::span(current->items, current->count)) {
				if (item != data) next->items[next->count++] = item;
			}
			m_Live.store(std::move(next));
			m_LiveCount.fetch_sub(1, std::memory_order_acq_rel);
		}

		std::unique_ptr<Snapshot::Slab> TakeSlab(size_t pages) {
			std::unique_ptr<Snapshot::Slab> slab;
			{
				std::lock_guard lock(m_SlabMutex);
				const auto fit = std::find_if(m_IdleSlabs.begin(), m_IdleSlabs.end(), [pages](const auto& candidate) { return candidate->capacity >= pages; });
				if (fit != m_IdleSlabs.end()) {
					slab = std::move(*fit);
					m_IdleSlabs.erase(fit);
				}
			}
			if (!slab) {
				slab = std::make_unique<Snapshot::Slab>();
				slab->capacity = pages + pages / 4;
				slab->pages = static_cast<Snapshot::Page*>(MapPages(slab->capacity * Snapshot::c_PageBytes));
				if (!slab->pages) throw std::runtime_error("could not map a Lua heap copy");
			}
			slab->next.store(0, std::memory_order_relaxed);
			return slab;
		}
		void GiveSlab(std::unique_ptr<Snapshot::Slab> slab) {
			std::lock_guard lock(m_SlabMutex);
			if (m_IdleSlabs.size() < 2) m_IdleSlabs.push_back(std::move(slab));
		}

		void Initialize() {
#if !LJ_GC64
			throw std::runtime_error("frozen Lua heap capture requires the GC64 allocator interface");
#else
			Reserve();
			InstallFaultHandler();
			Registry::Add(this);
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
		void Reserve() {
			void* base = VirtualAlloc(nullptr, c_ReserveBytes, MEM_RESERVE, PAGE_NOACCESS);
			if (!base) throw std::runtime_error("could not reserve the tracked Lua heap");
			m_Base = reinterpret_cast<uintptr_t>(base);
		}
		bool Commit(size_t bytes) noexcept {
			return VirtualAlloc(reinterpret_cast<void*>(m_Base + m_Committed.load(std::memory_order_relaxed)), bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
		}
		void Release() noexcept {
			if (m_Base) VirtualFree(reinterpret_cast<void*>(m_Base), 0, MEM_RELEASE);
			m_Base = 0;
		}
		bool Protect(size_t bytes) noexcept {
			DWORD previous = 0;
			return bytes == 0 || VirtualProtect(reinterpret_cast<void*>(m_Base), bytes, PAGE_READONLY, &previous) != 0;
		}
		static void InstallFaultHandler() {
			static std::once_flag once;
			std::call_once(once, [] {
				if (!AddVectoredExceptionHandler(1, &OnFault)) throw std::runtime_error("could not install the Lua heap fault handler");
			});
		}
		// The VM's first write to a protected page: every live snapshot still reading it live gets the
		// page as it is now, then the write goes through.
		static LONG WINAPI OnFault(EXCEPTION_POINTERS* info) noexcept {
			const EXCEPTION_RECORD* record = info->ExceptionRecord;
			if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 || record->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
			const uintptr_t address = static_cast<uintptr_t>(record->ExceptionInformation[1]);
			HeapOwner* owner = Registry::Find(address);
			if (!owner) return EXCEPTION_CONTINUE_SEARCH;
			const size_t index = (address - owner->m_Base) / Snapshot::c_PageBytes;
			const uintptr_t page = owner->m_Base + index * Snapshot::c_PageBytes;
			const auto live = owner->m_Live.load();
			for (Snapshot::Data* data: std::span(live->items, live->count)) {
				if (index >= data->pageCount || data->slots[index].load(std::memory_order_acquire)) continue;
				data->faults.fetch_add(1, std::memory_order_relaxed);
				Snapshot::Page* copy = data->slab->Take();
				if (!copy) { data->exhausted.store(true, std::memory_order_relaxed); continue; }
				std::memcpy(copy->bytes, reinterpret_cast<const void*>(page), Snapshot::c_PageBytes);
				const Snapshot::Page* expected = nullptr;
				if (data->slots[index].compare_exchange_strong(expected, copy, std::memory_order_acq_rel)) data->copied.fetch_add(1, std::memory_order_relaxed);
			}
			DWORD previous = 0;
			if (!VirtualProtect(reinterpret_cast<void*>(page), Snapshot::c_PageBytes, PAGE_READWRITE, &previous)) return EXCEPTION_CONTINUE_SEARCH;
			return EXCEPTION_CONTINUE_EXECUTION;
		}
#else
		static void* MapPages(size_t bytes) noexcept {
			void* pages = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			return pages == MAP_FAILED ? nullptr : pages;
		}
		static void Unmap(void* pages, size_t bytes) noexcept { munmap(pages, bytes); }
		void Reserve() {
			void* base = mmap(nullptr, c_ReserveBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
			if (base == MAP_FAILED) throw std::runtime_error("could not reserve the tracked Lua heap");
			m_Base = reinterpret_cast<uintptr_t>(base);
		}
		bool Commit(size_t bytes) noexcept {
			return mprotect(reinterpret_cast<void*>(m_Base + m_Committed.load(std::memory_order_relaxed)), bytes, PROT_READ | PROT_WRITE) == 0;
		}
		void Release() noexcept {
			if (m_Base) munmap(reinterpret_cast<void*>(m_Base), c_ReserveBytes);
			m_Base = 0;
		}
		static void InstallFaultHandler() {}
		// Without a fault handler every committed page is copied at the freeze; correct, not incremental.
		bool Protect(size_t bytes) noexcept {
			const auto live = m_Live.load();
			Snapshot::Data* data = live->count ? live->items[live->count - 1] : nullptr;
			if (!data) return false;
			for (size_t index = 0; index < bytes / Snapshot::c_PageBytes; ++index) {
				if (!data->Materialize(index)) return false;
			}
			return true;
		}
#endif

		void* Bump(size_t bytes) noexcept {
			if (bytes > c_ReserveBytes - m_Used) return nullptr;
			const size_t committed = m_Committed.load(std::memory_order_relaxed);
			if (m_Used + bytes > committed) {
				const size_t needed = m_Used + bytes - committed;
				const size_t step = std::max(c_CommitStep, (needed + c_CommitStep - 1) / c_CommitStep * c_CommitStep);
				if (committed + step > c_ReserveBytes || !Commit(step)) return nullptr;
				m_Committed.store(committed + step, std::memory_order_release);
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
		static void* Allocate(void* opaque, void* address, size_t previousSize, size_t size) noexcept {
			auto& owner = *static_cast<HeapOwner*>(opaque);
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

		friend struct Snapshot::Data;
		friend struct Snapshot::Slab;
	};

	inline Snapshot::Slab::~Slab() {
		if (pages) HeapOwner::Unmap(pages, capacity * c_PageBytes);
	}

	inline Snapshot::Data::~Data() {
		if (!owner) return;
		owner->Unpublish(this);
		if (slab) owner->GiveSlab(std::move(slab));
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
}
