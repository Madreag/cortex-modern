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
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
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

	// The VM's memory at one freeze: a page table over copies, shared with the freezes before it for
	// every page nothing wrote in between. Addresses identify the source VM; only the copies are read.
	class Snapshot {
	public:
		static constexpr size_t c_PageBytes = 4096;
		struct Page { std::byte bytes[c_PageBytes]; };

		Snapshot() = default;
		lua_State* State() const { return m_Data ? m_Data->state : nullptr; }
		uint64_t StateSerial() const { return m_Data ? m_Data->serial : 0; }
		size_t ByteCount() const { return m_Data ? m_Data->committed : 0; }
		size_t BlockCount() const { return m_Data ? m_Data->copied : 0; }
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
			size_t copied = 0;
			int64_t freezeUs = 0;
			std::atomic<int64_t> copyUs{0};
			std::vector<std::shared_ptr<const Page>> pages; // One per committed page; empty means never written.
			std::shared_future<void> ready; // The copy of this freeze's written pages, run off the simulation thread.
			mutable std::atomic<bool> copiedFlag{false};
			mutable std::deque<std::vector<std::byte>> assembled; // Read by the one worker that walks this snapshot.
			void WaitCopied() const {
				if (copiedFlag.load(std::memory_order_acquire)) return;
				if (ready.valid()) ready.wait();
				copiedFlag.store(true, std::memory_order_release);
			}
		};
		std::shared_ptr<const Data> m_Data;
		explicit Snapshot(std::shared_ptr<const Data> data) : m_Data(std::move(data)) {}

		static const Page& ZeroPage() {
			static const Page zero{};
			return zero;
		}
		const std::byte* PageBytes(size_t index) const {
			const auto& page = m_Data->pages[index];
			return page ? page->bytes : ZeroPage().bytes;
		}

		friend class HeapOwner;
	};

	// Owns the VM and the reservation every block comes from. The kernel keeps the page-written bits,
	// so a freeze copies the pages written since the freeze before it and shares the rest.
	class HeapOwner {
	public:
		static std::unique_ptr<HeapOwner> Create() {
			auto owner = std::unique_ptr<HeapOwner>(new HeapOwner());
			owner->Initialize();
			return owner;
		}

		~HeapOwner() {
			WaitCopy();
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

		using Submit = std::function<std::future<void>(std::function<void()>)>;

		// The caller must hold the VM's execution lock from the freeze until WaitCopy returns: the written
		// pages are read by the copy this submits, and nothing may write them before it is done.
		Snapshot Freeze(const Submit& submit) {
			const auto started = std::chrono::steady_clock::now();
			if (!m_State) throw std::runtime_error("a Lua heap capture has no state");
			void* allocatorData = nullptr;
			if (lua_getallocf(m_State, &allocatorData) != &Allocate || allocatorData != this)
				throw std::runtime_error("the Lua heap allocator changed after tracking began");
			WaitCopy();
			const size_t pageCount = m_Committed / Snapshot::c_PageBytes;
			m_Pages.resize(pageCount);
			m_Written.resize(pageCount);
			const size_t written = WrittenPages();
			auto data = std::make_shared<Snapshot::Data>();
			data->state = m_State;
			data->serial = G(m_State)->objserial;
			data->base = m_Base;
			data->committed = m_Committed;
			data->copied = written;
			data->pages = m_Pages;
			if (written) {
				std::shared_ptr<Slab> slab = TakeSlab(written);
				std::vector<void*> addresses(m_Written.begin(), m_Written.begin() + written);
				// Both page tables take the copies as they land; the simulation thread waits once, at the end of the world capture.
				auto copy = [this, data, slab, addresses = std::move(addresses)] {
					const auto copyStarted = std::chrono::steady_clock::now();
					for (size_t index = 0; index < addresses.size(); ++index) {
						Snapshot::Page& page = slab->pages[index];
						std::memcpy(page.bytes, addresses[index], Snapshot::c_PageBytes);
						const size_t at = (reinterpret_cast<uintptr_t>(addresses[index]) - m_Base) / Snapshot::c_PageBytes;
						auto shared = std::shared_ptr<const Snapshot::Page>(slab, &page);
						data->pages[at] = shared;
						m_Pages[at] = std::move(shared);
					}
					data->copyUs.store(MicrosecondsSince(copyStarted), std::memory_order_relaxed);
				};
				std::future<void> pending = submit ? submit(std::move(copy)) : std::future<void>();
				if (pending.valid()) {
					data->ready = pending.share();
					m_PendingCopy = data->ready;
				} else {
					copy();
				}
			}
			data->freezeUs = MicrosecondsSince(started);
			return Snapshot(std::move(data));
		}

		// Blocks until the last freeze's copy has landed; the VM may write its heap again after this.
		void WaitCopy() {
			if (m_PendingCopy.valid()) m_PendingCopy.wait();
			m_PendingCopy = {};
		}

	private:
		static constexpr size_t c_ReserveBytes = size_t(1) << 32; // Address space only; committed as the VM grows.
		static constexpr size_t c_CommitStep = size_t(1) << 20;
		static constexpr size_t c_LargeLimit = size_t(1) << 18; // Above this a block takes whole pages of its own.
		static constexpr size_t c_ClassCount = 64 + 56 + 62;

		HeapOwner() = default;
		std::unique_ptr<lua_State, decltype(&lua_close)> m_Bootstrap{nullptr, lua_close};
		lua_State* m_State = nullptr;
		uintptr_t m_Base = 0;
		size_t m_Committed = 0;
		size_t m_Used = 0;
		size_t m_Bytes = 0;
		size_t m_Blocks = 0;
		void* m_Free[c_ClassCount] = {};
		std::vector<std::pair<void*, size_t>> m_LargeFree; // Whole-page blocks given back, by their rounded size.
		std::vector<std::shared_ptr<const Snapshot::Page>> m_Pages;
		std::vector<void*> m_Written;

		// A copy buffer mapped straight from the system and kept for reuse: a reused one has its pages resident.
		struct Slab {
			Snapshot::Page* pages = nullptr;
			size_t capacity = 0;
			HeapOwner* owner = nullptr;
			~Slab();
		};
		static constexpr size_t c_IdleSlabs = 3;
		std::mutex m_SlabMutex;
		std::vector<std::unique_ptr<Slab>> m_IdleSlabs;
		std::shared_future<void> m_PendingCopy;
		std::shared_ptr<Slab> TakeSlab(size_t pages) {
			std::unique_ptr<Slab> slab;
			{
				std::lock_guard lock(m_SlabMutex);
				const auto fit = std::find_if(m_IdleSlabs.begin(), m_IdleSlabs.end(), [pages](const auto& candidate) { return candidate->capacity >= pages; });
				if (fit != m_IdleSlabs.end()) {
					slab = std::move(*fit);
					m_IdleSlabs.erase(fit);
				}
			}
			if (!slab) {
				slab = std::make_unique<Slab>();
				slab->owner = this;
				slab->capacity = pages + pages / 4;
				slab->pages = static_cast<Snapshot::Page*>(MapPages(slab->capacity * Snapshot::c_PageBytes));
				if (!slab->pages) throw std::runtime_error("could not map a Lua heap copy");
			}
			return std::shared_ptr<Slab>(std::move(slab));
		}
		void GiveSlab(Slab* slab) {
			std::lock_guard lock(m_SlabMutex);
			if (m_IdleSlabs.size() < c_IdleSlabs) {
				m_IdleSlabs.push_back(std::unique_ptr<Slab>(slab));
				return;
			}
			Unmap(slab->pages, slab->capacity * Snapshot::c_PageBytes);
			slab->pages = nullptr;
			delete slab;
		}

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

		void Initialize() {
#if !LJ_GC64
			throw std::runtime_error("frozen Lua heap capture requires the GC64 allocator interface");
#else
			Reserve();
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
			void* base = VirtualAlloc(nullptr, c_ReserveBytes, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
			if (!base) throw std::runtime_error("could not reserve the tracked Lua heap");
			m_Base = reinterpret_cast<uintptr_t>(base);
		}
		bool Commit(size_t bytes) noexcept {
			return VirtualAlloc(reinterpret_cast<void*>(m_Base + m_Committed), bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
		}
		void Release() noexcept {
			if (m_Base) VirtualFree(reinterpret_cast<void*>(m_Base), 0, MEM_RELEASE);
			m_Base = 0;
		}
		// The pages written since the last freeze, and the kernel's bits cleared for the next one.
		size_t WrittenPages() {
			if (m_Committed == 0) return 0;
			ULONG_PTR count = m_Written.size();
			DWORD granularity = 0;
			if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, reinterpret_cast<void*>(m_Base), m_Committed,
			        m_Written.data(), &count, &granularity) != 0 || granularity != Snapshot::c_PageBytes)
				throw std::runtime_error("the tracked Lua heap's written pages could not be read");
			return count;
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
			return mprotect(reinterpret_cast<void*>(m_Base + m_Committed), bytes, PROT_READ | PROT_WRITE) == 0;
		}
		void Release() noexcept {
			if (m_Base) munmap(reinterpret_cast<void*>(m_Base), c_ReserveBytes);
			m_Base = 0;
		}
		// Without page-written bits every committed page is copied; correct, not incremental.
		size_t WrittenPages() {
			for (size_t index = 0; index < m_Written.size(); ++index) m_Written[index] = reinterpret_cast<void*>(m_Base + index * Snapshot::c_PageBytes);
			return m_Written.size();
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
			owner->GiveSlab(kept);
			return;
		}
		HeapOwner::Unmap(pages, capacity * Snapshot::c_PageBytes);
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
		ForEachUserdata(state, true, false, visit);
	}
}
