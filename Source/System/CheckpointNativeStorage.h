#pragma once

#include "CheckpointPagePool.h"
#include "SaverThreadMark.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace RTE {

	// Saved resources and native multiplayer values use pages without moving their identities.
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
		class WorldAllocationScope {
		public:
			explicit WorldAllocationScope(bool enabled);
			~WorldAllocationScope();
			WorldAllocationScope(const WorldAllocationScope&) = delete;
			WorldAllocationScope& operator=(const WorldAllocationScope&) = delete;
		private:
			std::optional<bool> m_Previous;
		};
		// Manager headers use prepared storage; their pointees stay on frozen pages.
		struct RootRange { const void* source; const void* view; size_t bytes; bool privateValues = false; };
		template<class T> class Root {
		public:
			explicit Root(const T* source) : m_Source(source) {}
			Root(const Root&) = delete;
			Root& operator=(const Root&) = delete;
			void Freeze() noexcept { std::memcpy(m_Bytes.data(), m_Source, m_Bytes.size()); }
			RootRange Range() const { return {m_Source, m_Bytes.data(), m_Bytes.size()}; }
		private:
			const T* m_Source;
			alignas(T) std::array<std::byte, sizeof(T)> m_Bytes;
		};
		struct PrivateValues;
		struct ReadLookup;
		struct ReadViews {
			bool active = false;
			std::span<const std::shared_ptr<const CheckpointPagePool::Snapshot>> pages;
			std::span<const RootRange> roots;
			std::shared_ptr<PrivateValues> privateValues;
			std::shared_ptr<const void> owner;
			/// The open scope's built lookups, shared by a scope opened on these same views.
			std::shared_ptr<const ReadLookup> lookup;
		};
		struct ReadState;
		class ReadScope {
		public:
			explicit ReadScope(std::span<const std::shared_ptr<const CheckpointPagePool::Snapshot>> pages, std::span<const RootRange> roots = {});
			explicit ReadScope(ReadViews views);
			~ReadScope();
			ReadScope(const ReadScope&) = delete;
			ReadScope& operator=(const ReadScope&) = delete;
		private:
			CaptureScope m_Capture;
			std::shared_ptr<ReadState> m_State;
			ReadState* m_Previous;
		};
		static bool Enabled(bool retained = false);
		static bool RetainingAllocations() noexcept;
		static bool WorldEnabled() noexcept;
		static bool SetWorldEnabled(bool enabled) noexcept;
		/// Starts fencing new owned pages in the background while a world is captured, a slice at a time.
		static void StartSettling();
		/// Holds the settler's cooling passes while a save reads its capture: a page written meanwhile already has its saved
		/// bytes, so fencing it again would only cost the simulation another write fault.
		class CoolingPause {
		public:
			CoolingPause();
			~CoolingPause();
			CoolingPause(const CoolingPause&) = delete;
			CoolingPause& operator=(const CoolingPause&) = delete;
		};
		static bool Reading() { return s_Readers.load(std::memory_order_relaxed) != 0 && s_Read != nullptr; }
		static void RememberPrivate(void* address, size_t bytes, bool borrowed = false);
		static void BorrowPrivate(const void* address) noexcept;
		static bool PrivateBorrowed(const void* address) noexcept;
		static bool ForgetPrivate(void* address) noexcept;
		static bool ForgetPrivate(const std::shared_ptr<PrivateValues>& values, void* address) noexcept;
		static const void* PrivateAllocation(const void* address) noexcept;
		static bool Private(const void* address, size_t bytes = 1) noexcept;
		static size_t PrivateAllocationCount() noexcept;
		static void* AllocatePrivate(size_t bytes, size_t alignment, bool aligned = false);
		static ReadViews CurrentViews();
		static const void* RootView(const void* source, size_t bytes = 1);
		static void* Allocate(size_t bytes, size_t alignment);
		static bool Deallocate(void* address) noexcept;
		static bool Owns(const void* address);
		/// Slot pool, free slot and large buffer bytes, and how much grew inside explicit owned scopes.
		static std::string InventoryCensus();
		/// Test lever CCCP_TEST_STORAGE_COSTS: the owned allocations, frees and block growth so far and the time inside them; empty unset.
		static std::string StorageCosts();
		class Prepared {
		public:
			bool Current() const noexcept;
			bool Arm() const;
			void Cancel() const noexcept;
			std::span<const std::shared_ptr<const CheckpointPagePool::Snapshot>> Pages() const { return m_Pages; }
		private:
			friend class CheckpointNativeStorage;
			uint64_t m_Generation = 0;
			std::vector<std::shared_ptr<const CheckpointPagePool::Snapshot>> m_Pages;
		};
		static std::unique_ptr<Prepared> PrepareInventory();
		static std::vector<std::shared_ptr<const CheckpointPagePool::Snapshot>> Prepare();
		static bool ReadBytes(const void* source, void* target, size_t bytes);
		static const void* View(const void* source, size_t bytes);
		static const void* ViewObject(const void* source);
		template<class T> static T* Source(T* source) {
			// A thread that neither reads frozen views nor writes saves sees the live object; with neither anywhere, no thread needs its own look.
			if (s_Readers.load(std::memory_order_relaxed) == 0 && SaverThreadMark::marked.load(std::memory_order_relaxed) == 0) return source;
			if (!Reading() && !SaverThreadMark::current) return source;
			return static_cast<T*>(const_cast<void*>(ViewObject(source)));
		}
		template<class T> static T& PrivateValue(T* source) {
			if (!Reading()) return *source;
			T* value = Source(source);
			if (!Private(value, sizeof(T))) throw std::logic_error("a saved getter needs private native values before changing them");
			return *value;
		}
		static std::shared_ptr<const CheckpointPagePool::Snapshot> PagesFor(const void* source, size_t bytes);
		static const void* Original(const void* view);
		static bool IsView(const void* address);
		static const std::string* ReadString(const char* source, size_t bytes);
		static std::shared_ptr<const void> ReadIndex(const void* source, const void* kind, const std::function<std::shared_ptr<const void>()>& build);
		static std::string SelfTestMismatch();
		/// Empty when a thread made with a library's small stack (FMOD takes 96 KB for its output and stream threads) keeps most of
		/// it: glibc places every module's static thread-locals at the top of each thread's stack.
		static std::string ThreadStackSelfTestMismatch();
	private:
		static thread_local ReadState* s_Read;
		static inline std::atomic<int> s_Readers{0}; //!< Read scopes open on all threads.
	};

	class CheckpointNativeAllocated {
	public:
		static void* operator new(size_t bytes) {
			if (CheckpointNativeStorage::Reading()) return CheckpointNativeStorage::AllocatePrivate(bytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
			return CheckpointNativeStorage::Enabled() ? CheckpointNativeStorage::Allocate(bytes, __STDCPP_DEFAULT_NEW_ALIGNMENT__) : ::operator new(bytes);
		}
		static void operator delete(void* address) noexcept { if (!CheckpointNativeStorage::Deallocate(address)) ::operator delete(address); }
		static void* operator new(size_t bytes, std::align_val_t alignment) {
			if (CheckpointNativeStorage::Reading()) return CheckpointNativeStorage::AllocatePrivate(bytes, static_cast<size_t>(alignment), true);
			return CheckpointNativeStorage::Enabled() ? CheckpointNativeStorage::Allocate(bytes, static_cast<size_t>(alignment)) : ::operator new(bytes, alignment);
		}
		static void operator delete(void* address, std::align_val_t alignment) noexcept { if (!CheckpointNativeStorage::Deallocate(address)) ::operator delete(address, alignment); }
		static void* operator new[](size_t bytes) { return operator new(bytes); }
		static void operator delete[](void* address) noexcept { operator delete(address); }
		static void* operator new[](size_t bytes, std::align_val_t alignment) { return operator new(bytes, alignment); }
		static void operator delete[](void* address, std::align_val_t alignment) noexcept { operator delete(address, alignment); }
		static void* operator new(size_t, void* address) noexcept { return address; }
		static void operator delete(void*, void*) noexcept {}
	};

	template<class T> class CheckpointNativeAllocator {
	public:
		using value_type = T;
		using is_always_equal = std::true_type;
		using propagate_on_container_move_assignment = std::true_type;
		CheckpointNativeAllocator() noexcept : m_Owned(CheckpointNativeStorage::RetainingAllocations()) {}
		template<class U> CheckpointNativeAllocator(const CheckpointNativeAllocator<U>& source) noexcept : m_Owned(source.m_Owned) {}
		CheckpointNativeAllocator select_on_container_copy_construction() const noexcept { return {}; }
		[[nodiscard]] T* allocate(size_t count) {
			if (count > (std::numeric_limits<size_t>::max)() / sizeof(T)) throw std::bad_array_new_length();
			if (CheckpointNativeStorage::Reading()) {
				// The STL keeps its own alignment headers for large arrays.
				T* address = std::allocator<T>{}.allocate(count);
				try { CheckpointNativeStorage::RememberPrivate(address, count * sizeof(T)); }
				catch (...) { std::allocator<T>{}.deallocate(address, count); throw; }
				return address;
			}
			if (CheckpointNativeStorage::Enabled(m_Owned)) return static_cast<T*>(CheckpointNativeStorage::Allocate(count * sizeof(T), alignof(T)));
			return std::allocator<T>{}.allocate(count);
		}
		void deallocate(T* address, size_t count) noexcept {
			if (!CheckpointNativeStorage::Deallocate(address)) std::allocator<T>{}.deallocate(address, count);
		}
		template<class U> bool operator==(const CheckpointNativeAllocator<U>&) const noexcept { return true; }
	private:
		template<class> friend class CheckpointNativeAllocator;
		bool m_Owned;
	};

	template<class T, class... Args> std::shared_ptr<T> MakeCheckpointNativeShared(Args&&... args) {
		if (CheckpointNativeStorage::Reading() || CheckpointNativeStorage::Enabled()) return std::allocate_shared<T>(CheckpointNativeAllocator<T>{}, std::forward<Args>(args)...);
		return std::make_shared<T>(std::forward<Args>(args)...);
	}
}
