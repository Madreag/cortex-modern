#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace RTE {

	/// Page-isolated native storage whose frozen readers survive writes and pool reuse.
	class CheckpointPagePool {
		struct Block;
		struct Copy;

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
			bool Contains(const void* source, size_t bytes) const;
			/// Borrowed fields still match the live capture until a page has opened.
			bool CanBorrow(const void* source, size_t bytes) const;
			/// Materializes the remaining pages on the calling worker.
			void Drain() const;
			/// Fences the pages of a snapshot frozen without its fences; nothing may write them until this returns.
			void Arm() const;
			Costs Cost() const;
		private:
			friend class CheckpointPagePool;
			friend class Allocation;
			struct Part { std::shared_ptr<Block> block; std::shared_ptr<Copy> copy; };
			std::vector<Part> m_Parts;
		};
		class Allocation {
		public:
			explicit Allocation(size_t bytes);
			void* Data() const;
			size_t Bytes() const;
			bool Contains(const void* source, size_t bytes) const;
			std::shared_ptr<const Snapshot> Freeze() const;
		private:
			std::shared_ptr<Block> m_Block;
		};

		/// Appends slots in allocation order; the caller owns its usual free-list order.
		void Grow(size_t slotBytes, size_t minimumSlots, std::vector<void*>& free);
		bool Contains(const void* address) const;
		/// @param arm False leaves the fences to Snapshot::Arm, which must run before anything writes the pages again.
		std::shared_ptr<const Snapshot> Freeze(bool arm = true) const;
		static std::string SelfTestMismatch();
	private:
		std::vector<std::shared_ptr<Block>> m_Blocks;
	};
}
