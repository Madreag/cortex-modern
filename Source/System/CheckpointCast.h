#pragma once

#include "CheckpointArchive.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <typeinfo>
#include <vector>

namespace RTE {

	/// Reuses native RTTI adjustments while joined checkpoint readers hold the world.
	template<class Target, class Source> Target* CheckpointCast(Source* source) {
		static_assert(std::is_polymorphic_v<Source>);
		if (!source || !CheckpointWriter::BatchEnabled()) return dynamic_cast<Target*>(source);
		const std::type_info& type = typeid(*source);
		const auto* complete = static_cast<const char*>(dynamic_cast<const void*>(source));
		const ptrdiff_t from = reinterpret_cast<const char*>(source) - complete;
		struct Entry {
			const std::type_info* type;
			ptrdiff_t from;
			std::optional<ptrdiff_t> to;
		};
		struct Cache {
			std::vector<Entry> entries;
			size_t last = 0;
		};
		static thread_local Cache cache;
		const auto matches = [&](const Entry& entry) { return entry.from == from && entry.type == &type; };
		if (cache.entries.empty() || !matches(cache.entries[cache.last])) {
			const auto found = std::find_if(cache.entries.begin(), cache.entries.end(), matches);
			if (found != cache.entries.end()) cache.last = static_cast<size_t>(found - cache.entries.begin());
			else {
				// Equivalent type records from separate modules share the adjustment after their first lookup.
				const auto equivalent = std::find_if(cache.entries.begin(), cache.entries.end(), [&](const Entry& entry) {
					return entry.from == from && *entry.type == type;
				});
				std::optional<ptrdiff_t> to;
				if (equivalent != cache.entries.end()) to = equivalent->to;
				else if (const Target* target = dynamic_cast<Target*>(source)) to = reinterpret_cast<const char*>(target) - complete;
				cache.entries.push_back({&type, from, to});
				cache.last = cache.entries.size() - 1;
			}
		}
		// The source subobject distinguishes repeated and virtual bases of the same complete type.
		const auto& to = cache.entries[cache.last].to;
		return to ? reinterpret_cast<Target*>(const_cast<char*>(complete + *to)) : nullptr;
	}
}
