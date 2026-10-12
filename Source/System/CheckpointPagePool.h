#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace RTE {

	/// Page-isolated native storage whose frozen readers survive writes and pool reuse.
	class CheckpointPagePool {
		struct Block;
		struct Copy;
		struct Registry;
		struct ArmGate;
		static std::vector<std::shared_ptr<Block>> LiveBlocks();

	public:
		class Allocation;
		struct Costs {
			uint64_t bytes = 0;
			int64_t workerUs = 0, simFaultUs = 0, otherFaultUs = 0;
			uint64_t simFaults = 0, otherFaults = 0;
		};
		class Snapshot {
		public:
			~Snapshot();
			/// Copies only from frozen pages; false means the address is outside this pool.
			bool Read(const void* source, void* destination, size_t bytes) const;
			std::span<const std::byte> ReadBytes(const void* source, size_t bytes) const;
			const void* ReadAllocation(const void* source) const;
			bool Contains(const void* source, size_t bytes) const;
			const void* Original(const void* view, size_t bytes = 1) const;
			/// Borrowed fields still match the live capture until a page has opened.
			bool CanBorrow(const void* source, size_t bytes) const;
			/// Materializes the remaining pages on the calling worker.
			void Drain() const;
			/// Makes prepared shadow pages resident before any live page is fenced.
			void WarmShadow() const;
			/// Fences prepared ranges without allocating; writers pause only until this returns.
			void Arm() const;
			void Cancel() const noexcept;
			Costs Cost() const;
			size_t RangeCount() const { return m_Ranges.size(); }
			size_t RangeBytes() const { size_t bytes = 0; for (const Range& range: m_Ranges) bytes += range.bytes; return bytes; }
			/// What an inventory boundary did: pages copied as written, pages fenced as unseen.
			std::pair<size_t, size_t> ArmedPages() const;
		private:
			friend class CheckpointPagePool;
			friend class Allocation;
			struct Part { std::shared_ptr<Block> block; std::shared_ptr<Copy> copy; };
			std::vector<Part> m_Parts;
			std::vector<size_t> m_Views;
			struct Range { uintptr_t first; size_t bytes; };
			std::vector<Range> m_Ranges;
			std::shared_ptr<ArmGate> m_ArmGate;
			void Index();
		};
		class Allocation {
		public:
			explicit Allocation(size_t bytes, bool inventory = false);
			void* Data() const;
			size_t Bytes() const;
			bool Contains(const void* source, size_t bytes) const;
			std::shared_ptr<const Snapshot> Freeze(bool arm = true) const;
			void IncludeInInventory() const;
			/// Retires the live range while saved readers retain its pages.
			void ExcludeFromInventory() const noexcept;
		private:
			std::shared_ptr<Block> m_Block;
		};

		/// Appends slots in allocation order; the caller owns its usual free-list order.
		std::pair<void*, size_t> Grow(size_t slotBytes, size_t minimumSlots, std::vector<void*>& free, size_t blockBytes = size_t{4} << 20);
		bool Contains(const void* address) const;
		/// @param arm False prepares writable ranges; Snapshot::Arm records their later tick boundary.
		std::shared_ptr<const Snapshot> Freeze(bool arm = true) const;
		void IncludeInInventory();
		static std::shared_ptr<const Snapshot> PrepareInventory(uint64_t& generation);
		static uint64_t InventoryGeneration() noexcept;
		/// Fences up to the given inventory pages no boundary has seen; returns how many such pages remain.
		static size_t SettleInventory(size_t pages);
		/// Fences again the written inventory pages that stopped changing since the last pass; returns how many.
		static size_t CoolInventory();
		static std::string SelfTestMismatch();
	private:
		std::vector<std::shared_ptr<Block>> m_Blocks;
		size_t m_Slots = 0;
		bool m_InInventory = false;
		static void Register(const std::shared_ptr<Block>& block);
		static void Unregister(const std::shared_ptr<Block>& block) noexcept;
	};
}
