#pragma once

#include "CheckpointPagePool.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>

namespace RTE {

	// Only native multiplayer allocations enter these pages; ordinary allocations keep their allocator.
	class CheckpointNativeStorage {
	public:
		class AllocationScope {
		public:
			explicit AllocationScope(bool enabled);
			~AllocationScope();
			AllocationScope(const AllocationScope&) = delete;
			AllocationScope& operator=(const AllocationScope&) = delete;
		private:
			bool m_Previous;
		};
		static bool Enabled();
		static void* Allocate(size_t bytes, size_t alignment);
		static bool Deallocate(void* address) noexcept;
		static bool Owns(const void* address);
		static std::vector<std::shared_ptr<const CheckpointPagePool::Snapshot>> Prepare();
		static std::string SelfTestMismatch();
	};

	template<class T> class CheckpointNativeAllocator {
	public:
		using value_type = T;
		using is_always_equal = std::true_type;
		using propagate_on_container_move_assignment = std::true_type;
		CheckpointNativeAllocator() noexcept = default;
		template<class U> CheckpointNativeAllocator(const CheckpointNativeAllocator<U>&) noexcept {}
		[[nodiscard]] T* allocate(size_t count) {
			if (count > (std::numeric_limits<size_t>::max)() / sizeof(T)) throw std::bad_array_new_length();
			if (CheckpointNativeStorage::Enabled()) return static_cast<T*>(CheckpointNativeStorage::Allocate(count * sizeof(T), alignof(T)));
			return std::allocator<T>{}.allocate(count);
		}
		void deallocate(T* address, size_t count) noexcept {
			if (!CheckpointNativeStorage::Deallocate(address)) std::allocator<T>{}.deallocate(address, count);
		}
		template<class U> bool operator==(const CheckpointNativeAllocator<U>&) const noexcept { return true; }
	};
}
