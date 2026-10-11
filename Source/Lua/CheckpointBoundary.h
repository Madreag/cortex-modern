#pragma once

#include "CheckpointLuaHeap.h"
#include "CheckpointNativeStorage.h"

#include <bit>

namespace RTE::CheckpointLua {
	// The saver prepares ranges and header storage while the live pages stay writable.
	class Boundary {
	public:
		struct Root {
			const void* source;
			size_t bytes;
			size_t alignment;
			template<class T> static Root Of(const T& source) { return {&source, sizeof(T), alignof(T)}; }
		};
		static std::unique_ptr<Boundary> Prepare(std::span<const lua_State* const> states, std::span<const Root> roots) {
			auto result = std::unique_ptr<Boundary>(new Boundary);
			size_t bytes = 0, alignment = 1;
			for (const Root& root: roots) {
				if (!root.source || !root.bytes || !std::has_single_bit(root.alignment))
					throw std::invalid_argument("invalid checkpoint boundary header");
				alignment = (std::max)(alignment, root.alignment);
				if (bytes > (std::numeric_limits<size_t>::max)() - root.alignment + 1)
					throw std::bad_array_new_length();
				bytes = (bytes + root.alignment - 1) & ~(root.alignment - 1);
				if (root.bytes > (std::numeric_limits<size_t>::max)() - bytes) throw std::bad_array_new_length();
				bytes += root.bytes;
			}
			CheckpointFailure::Check(CheckpointFailure::Point::NativeObjects);
			if (bytes > (std::numeric_limits<size_t>::max)() - alignment + 1) throw std::bad_array_new_length();
			result->m_HeaderBytes.reset(bytes ? new std::byte[bytes + alignment - 1] : nullptr);
			const auto start = (reinterpret_cast<uintptr_t>(result->m_HeaderBytes.get()) + alignment - 1) & ~(alignment - 1);
			result->m_Roots.reserve(roots.size());
			size_t offset = 0;
			for (const Root& root: roots) {
				offset = (offset + root.alignment - 1) & ~(root.alignment - 1);
				result->m_Roots.push_back({root.source, reinterpret_cast<const void*>(start + offset), root.bytes});
				offset += root.bytes;
			}
			result->m_Heaps.reserve(states.size());
			for (const lua_State* state: states) {
				const auto lease = HeapOwner::LeasePreparation(const_cast<lua_State*>(state));
				if (!lease) throw std::runtime_error("checkpoint Lua heap has no preparation owner");
				result->m_Heaps.push_back({lease, lease.Prepare()});
			}
			result->m_Native = CheckpointNativeStorage::PrepareInventory();
			return result;
		}
		~Boundary() { Cancel(); }
		Boundary(const Boundary&) = delete;
		Boundary& operator=(const Boundary&) = delete;

		bool Current() const noexcept {
			if (m_Status.load(std::memory_order_acquire) != Status::Prepared || !m_Native || !m_Native->Current()) return false;
			for (const Heap& heap: m_Heaps) if (!heap.lease.Current(*heap.prepared)) return false;
			return true;
		}
		bool Arm() {
			if (!Current()) return false;
			Status expected = Status::Prepared;
			if (!m_Status.compare_exchange_strong(expected, Status::Arming, std::memory_order_acq_rel)) return false;
			try {
				if (!m_Native->Arm()) { Cancel(); return false; }
				for (Heap& heap: m_Heaps) if (!heap.lease.Arm(*heap.prepared)) { Cancel(); return false; }
				for (const auto& root: m_Roots) std::memcpy(const_cast<void*>(root.view), root.source, root.bytes);
				m_Status.store(Status::Armed, std::memory_order_release);
				return true;
			} catch (...) { Cancel(); throw; }
		}
		void Drain() {
			if (m_Status.load(std::memory_order_acquire) != Status::Armed) throw std::logic_error("checkpoint boundary is not armed");
			try {
				for (const auto& pages: m_Native->Pages()) pages->Drain();
				for (Heap& heap: m_Heaps) heap.prepared->Drain();
				m_Status.store(Status::Frozen, std::memory_order_release);
			} catch (...) { Cancel(); throw; }
		}
		void Cancel() noexcept {
			const Status previous = m_Status.exchange(Status::Cancelled, std::memory_order_acq_rel);
			if (previous == Status::Cancelled) return;
			if (m_Native) m_Native->Cancel();
			for (Heap& heap: m_Heaps) heap.prepared->Cancel();
		}
		CheckpointNativeStorage::ReadViews NativeViews() const {
			if (m_Status.load(std::memory_order_acquire) != Status::Frozen) throw std::logic_error("checkpoint boundary is not drained");
			return {true, m_Native->Pages(), m_Roots};
		}
		Snapshot LuaHeap(size_t index) const {
			if (m_Status.load(std::memory_order_acquire) != Status::Frozen) throw std::logic_error("checkpoint boundary is not drained");
			return m_Heaps.at(index).prepared->Image();
		}
		size_t StateCount() const { return m_Heaps.size(); }
		static std::string SelfTestMismatch() {
			CheckpointNativeStorage::AllocationScope allocations(true);
			auto state = HeapOwner::Create();
			auto other = HeapOwner::Create();
			lua_State* const lua = state->State();
			lua_State* const otherLua = other->State();
			struct Value : CheckpointNativeAllocated { uint64_t number = 7; };
			auto value = std::make_unique<Value>();
			const Value* const identity = value.get();
			struct Header { const Value* value; uint64_t tick; } header{identity, 1};
			const std::array<const lua_State*, 2> states{lua, otherLua};
			const std::array roots{Root::Of(header)};
			ProtectedCall(lua, [&] { lua_pushinteger(lua, 11); lua_setglobal(lua, "boundaryValue"); });
			auto prepared = std::async(std::launch::async, [&] { return Prepare(states, roots); }).get();
			header.tick = 2; value->number = 13;
			ProtectedCall(lua, [&] { lua_pushinteger(lua, 17); lua_setglobal(lua, "boundaryValue"); });
			if (!prepared->Arm()) return "prepared native and Lua boundary refused";
			header.tick = 3; value->number = 19;
			ProtectedCall(lua, [&] { lua_pushinteger(lua, 23); lua_setglobal(lua, "boundaryValue"); });
			value.reset();
			std::async(std::launch::async, [&] { prepared->Drain(); }).get();
			{
				CheckpointNativeStorage::ReadScope read(prepared->NativeViews());
				const auto* frozenHeader = CheckpointNativeStorage::Source(&header);
				if (frozenHeader->tick != 2 || frozenHeader->value != identity || CheckpointNativeStorage::Source(identity)->number != 13)
					return "native boundary lost its tick or object identity";
				const TValue number = prepared->LuaHeap(0).GlobalValue("boundaryValue");
				if (!tvisnumber(&number) || numberVnum(&number) != 17) return "Lua boundary differs from the native tick";
			}
			prepared.reset();
			auto stale = std::async(std::launch::async, [&] { return Prepare(states, roots); }).get();
			const size_t growth = stale->m_Heaps[1].prepared->Image().ByteCount() + Snapshot::c_PageBytes;
			const std::string text(growth, 'x');
			ProtectedCall(otherLua, [&] { lua_pushlstring(otherLua, text.data(), text.size()); lua_setglobal(otherLua, "growth"); });
			if (stale->Current() || stale->Arm()) return "grown Lua heap kept a stale boundary";
			stale->Cancel();
			bool refused = false;
			{
				CheckpointFailure::Scope failure(CheckpointFailure::Point::NativeObjects);
				try { Prepare(states, roots); } catch (const std::bad_alloc&) { refused = failure.Triggered(); }
			}
			if (!refused) return "boundary header allocation failure was not refused";
			refused = false;
			{
				CheckpointFailure::Scope failure(CheckpointFailure::Point::NativePages);
				try { Prepare(states, roots); } catch (const std::bad_alloc&) { refused = failure.Triggered(); }
			}
			if (!refused) return "native preparation failure left the Lua fences published";
			auto retry = std::async(std::launch::async, [&] { return Prepare(states, roots); }).get();
			header.tick = 4;
			if (!retry->Arm()) return "boundary refusal prevented a retry";
			header.tick = 5;
			std::async(std::launch::async, [&] { retry->Drain(); }).get();
			CheckpointNativeStorage::ReadScope read(retry->NativeViews());
			return CheckpointNativeStorage::Source(&header)->tick == 4 ? std::string() : "retried boundary header changed";
		}
	private:
		Boundary() = default;
		enum class Status { Prepared, Arming, Armed, Frozen, Cancelled };
		struct Heap { HeapOwner::PreparationLease lease; std::shared_ptr<HeapOwner::Prepared> prepared; };
		std::atomic<Status> m_Status{Status::Prepared};
		std::unique_ptr<CheckpointNativeStorage::Prepared> m_Native;
		std::vector<Heap> m_Heaps;
		std::unique_ptr<std::byte[]> m_HeaderBytes;
		std::vector<CheckpointNativeStorage::RootRange> m_Roots;
	};
}
