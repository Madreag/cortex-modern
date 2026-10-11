#pragma once

#include "CheckpointPagePool.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
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
		class CaptureScope {
		public:
			explicit CaptureScope(bool enabled = true);
			~CaptureScope();
			CaptureScope(const CaptureScope&) = delete;
			CaptureScope& operator=(const CaptureScope&) = delete;
		private:
			bool m_Previous;
		};
		struct ReadState;
		class ReadScope {
		public:
			explicit ReadScope(std::span<const std::shared_ptr<const CheckpointPagePool::Snapshot>> pages);
			~ReadScope();
			ReadScope(const ReadScope&) = delete;
			ReadScope& operator=(const ReadScope&) = delete;
		private:
			CaptureScope m_Capture;
			std::unique_ptr<ReadState> m_State;
			ReadState* m_Previous;
		};
		static bool Enabled();
		static void* Allocate(size_t bytes, size_t alignment);
		static bool Deallocate(void* address) noexcept;
		static bool Owns(const void* address);
		static std::vector<std::shared_ptr<const CheckpointPagePool::Snapshot>> Prepare();
		static bool ReadBytes(const void* source, void* target, size_t bytes);
		static const void* View(const void* source, size_t bytes);
		static const void* Original(const void* view);
		static bool IsView(const void* address) { return Original(address) != address; }
		static const std::string* ReadString(const char* source, size_t bytes);
		static std::string SelfTestMismatch();
	private:
		static thread_local ReadState* s_Read;
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

	template<class T, class... Args> std::shared_ptr<T> MakeCheckpointNativeShared(Args&&... args) {
		if (CheckpointNativeStorage::Enabled()) return std::allocate_shared<T>(CheckpointNativeAllocator<T>{}, std::forward<Args>(args)...);
		return std::make_shared<T>(std::forward<Args>(args)...);
	}
}
