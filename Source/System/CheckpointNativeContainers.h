#pragma once

#include "CheckpointNativeStorage.h"
#include "CheckpointString.h"

#include <deque>
#include <list>
#include <map>
#include <queue>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RTE {

	template<class T> using CheckpointVector = std::vector<T, CheckpointNativeAllocator<T>>;
	template<class T> using CheckpointDeque = std::deque<T, CheckpointNativeAllocator<T>>;
	template<class T> using CheckpointList = std::list<T, CheckpointNativeAllocator<T>>;
	template<class T> using CheckpointQueue = std::queue<T, CheckpointDeque<T>>;
	template<class K, class Compare = std::less<K>> using CheckpointSet = std::set<K, Compare, CheckpointNativeAllocator<K>>;
	template<class K, class V, class Compare = std::less<K>> using CheckpointMap = std::map<K, V, Compare, CheckpointNativeAllocator<std::pair<const K, V>>>;
	template<class K, class Hash = std::hash<K>, class Equal = std::equal_to<K>> using CheckpointUnorderedSet = std::unordered_set<K, Hash, Equal, CheckpointNativeAllocator<K>>;
	template<class K, class V, class Hash = std::hash<K>, class Equal = std::equal_to<K>> using CheckpointUnorderedMap = std::unordered_map<K, V, Hash, Equal, CheckpointNativeAllocator<std::pair<const K, V>>>;

	/// Gives a node container made before checkpoint storage an allocator that keeps its links in storage, its nodes left where
	/// they are: some standard libraries allocate a container's end node when it is made, outside every frozen page.
	template<class Container> void RetainCheckpointLinks(Container& container) {
		if (container.get_allocator().Retained()) return;
		CheckpointNativeStorage::AllocationScope retained(true);
		Container owned;
		if constexpr (requires { owned.splice(owned.end(), container); }) owned.splice(owned.end(), container);
		else owned.merge(container);
		container = std::move(owned);
	}
}
