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
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace RTE::CheckpointLua {

	struct AllocationStats {
		size_t bytes = 0;
		size_t blocks = 0;
	};

	class HeapOwner;

	// Addresses identify the source VM; only the copied bytes may be read by the worker.
	class Snapshot {
	public:
		Snapshot() = default;
		lua_State* State() const { return m_Data ? m_Data->state : nullptr; }
		uint64_t StateSerial() const { return m_Data ? m_Data->serial : 0; }
		size_t ByteCount() const { return m_Data ? m_Data->byteCount : 0; }
		size_t BlockCount() const { return m_Data ? m_Data->blocks.size() : 0; }
		int64_t FreezeUs() const { return m_Data ? m_Data->freezeUs : 0; }
		int64_t CopyUs() const { return m_Data ? m_Data->copyUs : 0; }

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
			if (source == 0 || size > std::numeric_limits<uintptr_t>::max() - source)
				throw std::runtime_error("a Lua heap read has an invalid address range");
			BuildAddressIndex();
			const auto& ranges = m_Data->addressIndex;
			auto found = std::upper_bound(ranges.begin(), ranges.end(), source,
			    [](uintptr_t value, const Block& block) { return value < block.source; });
			if (found == ranges.begin()) throw std::runtime_error("a Lua heap read names an unrecorded allocation");
			--found;
			const uintptr_t within = source - found->source;
			if (within > found->size || size > found->size - within)
				throw std::runtime_error("a Lua heap read extends beyond its recorded allocation");
			return {m_Data->bytes.get() + found->offset + static_cast<size_t>(within), size};
		}

		std::string ReadString(const char* address, size_t size) const {
			const auto bytes = ReadBytes(address, size);
			return size ? std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()) : std::string();
		}

	private:
		struct Block {
			uintptr_t source = 0;
			size_t size = 0;
			size_t offset = 0;
		};
		struct Data {
			lua_State* state = nullptr;
			uint64_t serial = 0;
			size_t byteCount = 0;
			int64_t freezeUs = 0;
			int64_t copyUs = 0;
			std::unique_ptr<std::byte[]> bytes;
			std::vector<Block> blocks;
			mutable std::once_flag indexed;
			mutable std::vector<Block> addressIndex;
		};
		std::shared_ptr<const Data> m_Data;
		explicit Snapshot(std::shared_ptr<const Data> data) : m_Data(std::move(data)) {}

		void BuildAddressIndex() const {
			std::call_once(m_Data->indexed, [data = m_Data] {
				std::vector<Block> ranges = data->blocks;
				std::sort(ranges.begin(), ranges.end(), [](const Block& left, const Block& right) { return left.source < right.source; });
				uintptr_t previousEnd = 0;
				for (const Block& block: ranges) {
					if (block.source == 0 || block.source < previousEnd ||
					    block.size > std::numeric_limits<uintptr_t>::max() - block.source ||
					    block.offset > data->byteCount || block.size > data->byteCount - block.offset)
						throw std::runtime_error("a Lua heap snapshot has an invalid allocation index");
					previousEnd = block.source + block.size;
				}
				data->addressIndex = std::move(ranges);
			});
		}

		friend class HeapOwner;
	};

	// Owns the VM and the allocator arena from which every recorded block came.
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
		}
		HeapOwner(const HeapOwner&) = delete;
		HeapOwner& operator=(const HeapOwner&) = delete;
		HeapOwner(HeapOwner&&) = delete;
		HeapOwner& operator=(HeapOwner&&) = delete;

		lua_State* State() const { return m_State; }

		AllocationStats Stats() const {
			std::lock_guard lock(m_Mutex);
			return {m_Bytes, m_Blocks.size()};
		}

		// The caller must hold the VM's execution lock throughout the copy.
		Snapshot Freeze() const {
			const auto started = std::chrono::steady_clock::now();
			std::lock_guard lock(m_Mutex);
			if (!m_State) throw std::runtime_error("a Lua heap capture has no state");
			if (const char* error = m_TrackingFailure.load()) throw std::runtime_error(error);
			void* allocatorData = nullptr;
			if (lua_getallocf(m_State, &allocatorData) != &Allocate || allocatorData != this)
				throw std::runtime_error("the Lua heap allocator changed after tracking began");
			auto data = std::make_shared<Snapshot::Data>();
			data->state = m_State;
			data->serial = G(m_State)->objserial;
			data->byteCount = m_Bytes;
			data->blocks.reserve(m_Blocks.size());
			data->bytes.reset(new std::byte[m_Bytes]);
			const auto copyStarted = std::chrono::steady_clock::now();
			size_t offset = 0;
			for (const auto& [address, size]: m_Blocks) {
				if (!address || size > m_Bytes - offset) throw std::runtime_error("the Lua heap allocation ledger is inconsistent");
				data->blocks.push_back({reinterpret_cast<uintptr_t>(address), size, offset});
				std::memcpy(data->bytes.get() + offset, address, size);
				offset += size;
			}
			if (offset != m_Bytes) throw std::runtime_error("the Lua heap allocation byte count is inconsistent");
			data->copyUs = MicrosecondsSince(copyStarted);
			data->freezeUs = MicrosecondsSince(started);
			return Snapshot(std::move(data));
		}

	private:
		HeapOwner() = default;
		std::unique_ptr<lua_State, decltype(&lua_close)> m_Bootstrap{nullptr, lua_close};
		lua_State* m_State = nullptr;
		lua_Alloc m_Forward = nullptr;
		void* m_ForwardData = nullptr;
		mutable std::mutex m_Mutex;
		std::unordered_map<void*, size_t> m_Blocks;
		size_t m_Bytes = 0;
		std::atomic<const char*> m_TrackingFailure{nullptr};

		static int64_t MicrosecondsSince(std::chrono::steady_clock::time_point started) {
			return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
		}

		void Initialize() {
#if !LJ_GC64
			throw std::runtime_error("frozen Lua heap capture requires the GC64 allocator interface");
#else
			m_Bootstrap.reset(luaL_newstate());
			if (!m_Bootstrap) throw std::runtime_error("could not create the Lua allocator bootstrap");
			m_Forward = lua_getallocf(m_Bootstrap.get(), &m_ForwardData);
			if (!m_Forward) throw std::runtime_error("the Lua bootstrap supplied no allocator");
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

		static void* Allocate(void* opaque, void* address, size_t previousSize, size_t size) noexcept {
			auto& owner = *static_cast<HeapOwner*>(opaque);
			void* result = nullptr;
			bool forwarded = false;
			try {
				std::lock_guard lock(owner.m_Mutex);
				auto previous = owner.m_Blocks.end();
				if (address) {
					previous = owner.m_Blocks.find(address);
					if (previous == owner.m_Blocks.end()) {
						owner.m_TrackingFailure.store("the Lua allocator received an unrecorded allocation");
					} else if (previous->second != previousSize) {
						owner.m_TrackingFailure.store("the Lua allocator received an inconsistent allocation size");
					}
				}
				const size_t recordedSize = previous == owner.m_Blocks.end() ? 0 : previous->second;
				if (size == 0) {
					result = owner.m_Forward(owner.m_ForwardData, address, previousSize, 0);
					forwarded = true;
					if (previous != owner.m_Blocks.end()) owner.m_Blocks.erase(previous);
					owner.m_Bytes -= recordedSize;
					return result;
				}
				if (size > std::numeric_limits<size_t>::max() - (owner.m_Bytes - recordedSize)) return nullptr;
				// Reserve the ledger node before realloc can move the old allocation.
				if (previous == owner.m_Blocks.end()) previous = owner.m_Blocks.emplace(nullptr, 0).first;
				result = owner.m_Forward(owner.m_ForwardData, address, previousSize, size);
				forwarded = true;
				if (!result) {
					if (!previous->first) owner.m_Blocks.erase(previous);
					return nullptr;
				}
				auto record = owner.m_Blocks.extract(previous);
				record.key() = result;
				record.mapped() = size;
				if (!owner.m_Blocks.insert(std::move(record)).inserted)
					owner.m_TrackingFailure.store("the Lua allocator returned an overlapping allocation");
				owner.m_Bytes = owner.m_Bytes - recordedSize + size;
				return result;
			} catch (...) {
				if (forwarded) owner.m_TrackingFailure.store("the Lua allocation ledger could not retain an allocation");
				return forwarded ? result : nullptr;
			}
		}
	};
}
