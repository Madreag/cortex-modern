#pragma once

#include "CheckpointArchive.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace RTE::CheckpointLua {
	// Membership has no iteration order; contiguous slots keep the fenced walk local.
	class CaptureAddressSet {
	public:
		explicit CaptureAddressSet(std::pmr::memory_resource* resource, bool batch = CheckpointWriter::BatchEnabled()) : m_Slots(resource) {
			if (!batch) m_Original.emplace(resource);
		}
		size_t Size() const { return m_Original ? m_Original->size() : m_Size; }
		bool Contains(const void* pointer) const {
			if (m_Original) return m_Original->contains(pointer);
			if (!pointer) return m_Null;
			if (m_Slots.empty()) return false;
			return m_Slots[Slot(m_Slots, pointer)] == pointer;
		}
		bool Insert(const void* pointer) {
			if (m_Original) return m_Original->insert(pointer).second;
			if (!pointer) {
				if (m_Null) return false;
				m_Null = true; ++m_Size; return true;
			}
			if (m_Slots.empty()) Grow(8);
			size_t slot = Slot(m_Slots, pointer);
			if (m_Slots[slot]) return false;
			if (m_Size + 1 > m_Slots.size() / 2) {
				Reserve(m_Size + 1);
				slot = Slot(m_Slots, pointer);
			}
			m_Slots[slot] = pointer; ++m_Size; return true;
		}
		void Reserve(size_t count) {
			if (m_Original) { m_Original->reserve(count); return; }
			if (count > m_Slots.max_size() / 2) throw std::length_error("checkpoint address set is too large");
			size_t capacity = 8;
			while (capacity / 2 < count) {
				if (capacity > m_Slots.max_size() / 2) throw std::length_error("checkpoint address set is too large");
				capacity *= 2;
			}
			if (capacity > m_Slots.size()) Grow(capacity);
		}
	private:
		std::optional<std::pmr::unordered_set<const void*>> m_Original;
		std::pmr::vector<const void*> m_Slots;
		size_t m_Size = 0;
		bool m_Null = false;
		static size_t Slot(const std::pmr::vector<const void*>& slots, const void* pointer) {
			uint64_t hash = reinterpret_cast<uintptr_t>(pointer);
			hash ^= hash >> 33; hash *= UINT64_C(0xff51afd7ed558ccd);
			hash ^= hash >> 33; hash *= UINT64_C(0xc4ceb9fe1a85ec53);
			hash ^= hash >> 33;
			const size_t mask = slots.size() - 1;
			size_t slot = static_cast<size_t>(hash) & mask;
			while (slots[slot] && slots[slot] != pointer) slot = (slot + 1) & mask;
			return slot;
		}
		void Grow(size_t capacity) {
			std::pmr::vector<const void*> slots(capacity, nullptr, m_Slots.get_allocator().resource());
			for (const void* pointer: m_Slots) if (pointer) slots[Slot(slots, pointer)] = pointer;
			m_Slots.swap(slots);
		}
	};
}
