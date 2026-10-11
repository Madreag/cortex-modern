#pragma once

#include "CheckpointNativeStorage.h"

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
}
