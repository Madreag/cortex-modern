#pragma once

#include "CheckpointFrozenContainers.h"
#include "CheckpointNativeStorage.h"

#include "luabind/iterator_policy.hpp"

#include <concepts>
#include <cstddef>
#include <deque>
#include <iterator>
#include <memory>
#include <type_traits>

namespace RTE {

	template<class T> struct CheckpointIsDeque : std::false_type {};
	template<class T, class Allocator> struct CheckpointIsDeque<std::deque<T, Allocator>> : std::true_type {};

	/// Where an iterator stands in a frozen container: by its own fields when they say, else by the frozen element at its address.
	template<class Container, class Iterator> lua_Integer CheckpointFrozenIteratorIndex(const Container& frozen, const Iterator& position) {
#ifdef _LIBCPP_VERSION
		// This library's deque iterators read their block map to subtract.
		constexpr bool arithmetic = std::contiguous_iterator<Iterator>;
#else
		constexpr bool arithmetic = std::contiguous_iterator<Iterator> || CheckpointIsDeque<std::remove_cv_t<Container>>::value;
#endif
		if constexpr (arithmetic) {
			return static_cast<lua_Integer>(position - frozen.begin());
		} else {
			const auto values = CheckpointValues(frozen);
			// Only the node's address is formed here; the live element is never read.
			const void* target = std::addressof(*position);
			for (size_t index = 0; index < values.size(); ++index) {
				if (CheckpointNativeStorage::Original(std::addressof(values[index])) == target) return static_cast<lua_Integer>(index);
			}
			return static_cast<lua_Integer>(values.size());
		}
	}
} // namespace RTE

namespace luabind::detail {

	// (frozen range, owner) -> the table the live range's snapshot returns at the same tick.
	template<class Container, bool Owned> int iterator_frozen_snapshot(lua_State* L) {
		using range_type = iterator<Container, Owned>;
		const auto* range = static_cast<const range_type*>(lua_touserdata(L, 1));
		lua_newtable(L);
		lua_pushboolean(L, Owned);
		lua_setfield(L, -2, "owned");
		if constexpr (Owned) {
			// An owned container never changes once its range is made, and outlives every reader that started with it.
			Container& container = *range->container;
			lua_pushinteger(L, std::distance(container.begin(), range->first));
			lua_setfield(L, -2, "first");
			lua_newtable(L);
			int index = 0;
			for (auto item = range->first; item != range->last; ++item) {
				convert_to_lua(L, *item);
				object_rep* value = is_class_object(L, -1);
				if (value && !(value->flags() & object_rep::owner) && !lua_isnil(L, 2)) value->add_dependency(L, 2);
				lua_rawseti(L, -2, ++index);
			}
			lua_setfield(L, -2, "values");
			lua_pushinteger(L, index);
			lua_setfield(L, -2, "count");
		} else {
			const auto* frozen = static_cast<const Container*>(RTE::CheckpointNativeStorage::View(range->container, sizeof(Container)));
			lua_pushinteger(L, RTE::CheckpointFrozenIteratorIndex(*frozen, range->first));
			lua_setfield(L, -2, "first");
			lua_pushinteger(L, RTE::CheckpointFrozenIteratorIndex(*frozen, range->last));
			lua_setfield(L, -2, "last");
		}
		return 1;
	}
} // namespace luabind::detail
