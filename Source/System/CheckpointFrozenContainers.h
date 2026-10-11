#pragma once

#include "CheckpointNativeContainers.h"

#include <bit>
#include <cstring>
#include <iterator>
#include <stdexcept>

namespace RTE {
	std::string CheckpointFrozenContainersSelfTestMismatch();

	// These readers follow the library's links in frozen pages, without touching a live node.
	namespace CheckpointContainerDetail {
		template<class T> const T* Read(const T* address) {
			return static_cast<const T*>(CheckpointNativeStorage::View(address, sizeof(T)));
		}
		template<class Iterator> const void* Node(const Iterator& iterator) {
			if constexpr (requires { iterator._Ptr; }) return iterator._Ptr;
			else if constexpr (requires { iterator._M_node; }) return iterator._M_node;
			else if constexpr (requires { iterator._M_cur; }) return iterator._M_cur;
			else {
				static_assert(sizeof(Iterator) == sizeof(void*) && std::is_trivially_copyable_v<Iterator>);
				return std::bit_cast<const void*>(iterator);
			}
		}
		template<class Iterator> Iterator At(Iterator iterator, const void* address) {
			if constexpr (requires { iterator._Ptr; }) iterator._Ptr = static_cast<decltype(iterator._Ptr)>(const_cast<void*>(address));
			else if constexpr (requires { iterator._M_node; }) iterator._M_node = static_cast<decltype(iterator._M_node)>(const_cast<void*>(address));
			else if constexpr (requires { iterator._M_cur; }) iterator._M_cur = static_cast<decltype(iterator._M_cur)>(const_cast<void*>(address));
			else {
				static_assert(sizeof(Iterator) == sizeof(void*) && std::is_trivially_copyable_v<Iterator>);
				iterator = std::bit_cast<Iterator>(address);
			}
			return iterator;
		}
		template<class Container> auto End(const Container& container) {
#ifdef _MSVC_STL_VERSION
			if constexpr (requires { container._Unchecked_end()._Ptr; }) return container._Unchecked_end();
			else return container.end()._Unwrapped();
#else
			return container.end();
#endif
		}
		template<class Iterator> auto Value(Iterator iterator, const void* node) {
			// Dereferencing a node iterator locates its value; only Read accesses the value's bytes.
			return Read(std::addressof(*At(iterator, node)));
		}
		struct TreeLinks { const void* left; const void* right; const void* parent; };
		inline TreeLinks Tree(const void* node) {
#ifdef _MSVC_STL_VERSION
			struct Links { const void* left; const void* parent; const void* right; };
			const auto* links = Read(static_cast<const Links*>(node));
			return {links->left, links->right, links->parent};
#elif defined(__GLIBCXX__)
			const auto* links = Read(static_cast<const std::_Rb_tree_node_base*>(node));
			return {links->_M_left, links->_M_right, links->_M_parent};
#elif defined(_LIBCPP_VERSION)
			const auto* links = Read(static_cast<const std::__tree_node_base<void*>*>(node));
			return {links->__left_, links->__right_, links->__parent_};
#else
#error The native checkpoint reader needs this standard library's node links.
#endif
		}
		inline const void* ListNext(const void* node) {
#ifdef _LIBCPP_VERSION
			struct Links { const void* previous; const void* next; };
#else
			struct Links { const void* next; const void* previous; };
#endif
			return Read(static_cast<const Links*>(node))->next;
		}
		inline const void* HashNext(const void* node) { return *Read(static_cast<const void* const*>(node)); }
#ifdef _LIBCPP_VERSION
		template<class T> struct DequeHeader { const T* const* first; const T* const* begin; const T* const* end; const T* const* capacity; size_t start; size_t size; };
		template<class T, class A> DequeHeader<T> Deque(const std::deque<T, A>& source) {
			static_assert(std::is_empty_v<A> && sizeof(source) == sizeof(DequeHeader<T>));
			DequeHeader<T> header;
			std::memcpy(&header, &source, sizeof(header));
			if (header.size != source.size()) throw std::logic_error("unsupported frozen deque layout");
			return header;
		}
#endif
	}

	template<class T> class CheckpointFrozenRange {
	public:
		using value_type = T;
		struct Iterator {
			using iterator_category = std::forward_iterator_tag;
			using value_type = T;
			using difference_type = ptrdiff_t;
			using reference = const T&;
			using pointer = const T*;
			const T* const* at;
			reference operator*() const { return **at; }
			pointer operator->() const { return *at; }
			Iterator& operator++() { ++at; return *this; }
			Iterator operator++(int) { auto previous = *this; ++at; return previous; }
			bool operator==(const Iterator&) const = default;
		};
		explicit CheckpointFrozenRange(size_t size) { m_Values.reserve(size); }
		void Add(const T* value) { m_Values.push_back(value); }
		Iterator begin() const { return {m_Values.data()}; }
		Iterator end() const { return {m_Values.empty() ? m_Values.data() : m_Values.data() + m_Values.size()}; }
		size_t size() const { return m_Values.size(); }
		bool empty() const { return m_Values.empty(); }
		const T& operator[](size_t index) const { return *m_Values[index]; }
		const T& front() const { return *m_Values.front(); }
		const T& back() const { return *m_Values.back(); }
	private:
		std::vector<const T*> m_Values;
	};

	template<class T, class Allocator> auto CheckpointValues(const std::vector<T, Allocator>& source) {
		static_assert(!std::is_same_v<T, bool>);
		CheckpointFrozenRange<T> values(source.size());
		const T* first = source.data();
		if (!source.empty() && CheckpointNativeStorage::IsView(&source)) first = static_cast<const T*>(CheckpointNativeStorage::View(first, source.size() * sizeof(T)));
		for (size_t index = 0; index < source.size(); ++index) values.Add(first + index);
		return values;
	}

	template<class T, class Allocator> auto CheckpointValues(const std::deque<T, Allocator>& source) {
		CheckpointFrozenRange<T> values(source.size());
		if (!CheckpointNativeStorage::IsView(&source)) { for (const auto& value: source) values.Add(&value); return values; }
		if (source.empty()) return values;
#ifdef _MSVC_STL_VERSION
		const auto first = source._Unchecked_begin();
		const auto& header = *first._Mycont;
		const size_t blockSize = std::remove_cvref_t<decltype(header)>::_Block_size;
		for (size_t index = 0; index < source.size(); ++index) {
			const size_t at = first._Myoff + index;
			const T* block = *CheckpointContainerDetail::Read(header._Map + header._Getblock(at));
			values.Add(CheckpointContainerDetail::Read(block + at % blockSize));
		}
#elif defined(__GLIBCXX__)
		const auto first = source.begin();
		const size_t blockSize = first._M_last - first._M_first;
		const size_t offset = first._M_cur - first._M_first;
		for (size_t index = 0; index < source.size(); ++index) {
			const size_t at = offset + index;
			const T* block = *CheckpointContainerDetail::Read(first._M_node + at / blockSize);
			values.Add(CheckpointContainerDetail::Read(block + at % blockSize));
		}
#elif defined(_LIBCPP_VERSION)
		const auto header = CheckpointContainerDetail::Deque(source);
		const size_t blockSize = std::__deque_block_size<T, ptrdiff_t>::value;
		for (size_t index = 0; index < source.size(); ++index) {
			const size_t at = header.start + index;
			const T* block = *CheckpointContainerDetail::Read(header.begin + at / blockSize);
			values.Add(CheckpointContainerDetail::Read(block + at % blockSize));
		}
#endif
		return values;
	}

	template<class T, class Allocator> auto CheckpointValues(const std::list<T, Allocator>& source) {
		CheckpointFrozenRange<T> values(source.size());
		if (!CheckpointNativeStorage::IsView(&source)) { for (const auto& value: source) values.Add(&value); return values; }
		const auto iterator = CheckpointContainerDetail::End(source);
		const void* end = CheckpointNativeStorage::Original(CheckpointContainerDetail::Node(iterator));
		const void* node = CheckpointContainerDetail::ListNext(end);
		for (size_t index = 0; index < source.size(); ++index) {
			if (!node || node == end) throw std::logic_error("truncated frozen list");
			values.Add(CheckpointContainerDetail::Value(iterator, node));
			node = CheckpointContainerDetail::ListNext(node);
		}
		if (node != end) throw std::logic_error("frozen list count differs from its links");
		return values;
	}

	template<class Container> auto CheckpointTreeValues(const Container& source) {
		CheckpointFrozenRange<typename Container::value_type> values(source.size());
		if (!CheckpointNativeStorage::IsView(&source)) { for (const auto& value: source) values.Add(&value); return values; }
		if (source.empty()) return values;
		const auto iterator = CheckpointContainerDetail::End(source);
		const void* end = CheckpointNativeStorage::Original(CheckpointContainerDetail::Node(iterator));
#ifdef _LIBCPP_VERSION
		const void* node = *CheckpointContainerDetail::Read(static_cast<const void* const*>(end));
		while (const void* left = CheckpointContainerDetail::Tree(node).left) node = left;
#else
		const void* node = CheckpointContainerDetail::Tree(end).left;
#endif
		size_t steps = 0;
		for (size_t index = 0; index < source.size(); ++index) {
			if (!node || node == end) throw std::logic_error("truncated frozen tree");
			values.Add(CheckpointContainerDetail::Value(iterator, node));
			auto links = CheckpointContainerDetail::Tree(node);
			if (links.right && links.right != end) {
				node = links.right;
				for (;;) {
					if (++steps > source.size() * 3) throw std::logic_error("cyclic frozen tree");
					const void* left = CheckpointContainerDetail::Tree(node).left;
					if (!left || left == end) break;
					node = left;
				}
			} else {
				const void* parent = links.parent;
				while (parent != end && node == CheckpointContainerDetail::Tree(parent).right) {
					if (++steps > source.size() * 3) throw std::logic_error("cyclic frozen tree");
					node = parent; parent = CheckpointContainerDetail::Tree(node).parent;
				}
				node = parent;
			}
		}
		if (node != end) throw std::logic_error("frozen tree count differs from its links");
		return values;
	}
	template<class K, class C, class A> auto CheckpointValues(const std::set<K, C, A>& source) { return CheckpointTreeValues(source); }
	template<class K, class V, class C, class A> auto CheckpointValues(const std::map<K, V, C, A>& source) { return CheckpointTreeValues(source); }

	template<class Container> auto CheckpointHashValues(const Container& source) {
		CheckpointFrozenRange<typename Container::value_type> values(source.size());
		if (!CheckpointNativeStorage::IsView(&source)) { for (const auto& value: source) values.Add(&value); return values; }
		if (source.empty()) return values;
		const auto iterator = CheckpointContainerDetail::End(source);
		const void* end = CheckpointNativeStorage::Original(CheckpointContainerDetail::Node(iterator));
#ifdef _MSVC_STL_VERSION
		const void* node = CheckpointContainerDetail::HashNext(end);
#else
		const void* node = CheckpointContainerDetail::Node(source.begin());
#endif
		for (size_t index = 0; index < source.size(); ++index) {
			if (!node || node == end) throw std::logic_error("truncated frozen hash table");
			values.Add(CheckpointContainerDetail::Value(iterator, node));
			node = CheckpointContainerDetail::HashNext(node);
		}
		if (node != end) throw std::logic_error("frozen hash count differs from its links");
		return values;
	}
	template<class K, class H, class E, class A> auto CheckpointValues(const std::unordered_set<K, H, E, A>& source) { return CheckpointHashValues(source); }
	template<class K, class V, class H, class E, class A> auto CheckpointValues(const std::unordered_map<K, V, H, E, A>& source) { return CheckpointHashValues(source); }

	template<class Container, class Convert> Container CheckpointRebuildHash(const Container& source, Convert convert) {
		Container result(source.bucket_count(), source.hash_function(), source.key_eq(), source.get_allocator());
		result.max_load_factor(source.max_load_factor());
		const auto values = CheckpointValues(source);
#ifdef _MSVC_STL_VERSION
		const auto key = [](const auto& value) -> const auto& {
			if constexpr (requires { typename Container::mapped_type; }) return value.first;
			else return value;
		};
		// MSVC appends new buckets, but prepends each new value inside its bucket.
		for (size_t first = 0; first < values.size();) {
			size_t last = first + 1;
			const size_t bucket = result.bucket(key(values[first]));
			while (last < values.size() && result.bucket(key(values[last])) == bucket) ++last;
			for (size_t index = last; index != first;) result.insert(convert(values[--index]));
			first = last;
		}
#else
		// These libraries prepend both new buckets and new values inside a bucket.
		for (size_t index = values.size(); index != 0;) result.insert(convert(values[--index]));
#endif
		return result;
	}

	template<class T, class Container> const Container& CheckpointQueueValues(const std::queue<T, Container>& source) {
		struct Access : std::queue<T, Container> { static const Container& Get(const std::queue<T, Container>& queue) { return queue.*&Access::c; } };
		return Access::Get(source);
	}
	template<class T, class Container> auto CheckpointValues(const std::queue<T, Container>& source) { return CheckpointValues(CheckpointQueueValues(source)); }

	template<class T, class A, class Iterator> size_t CheckpointIteratorIndex(const std::list<T, A>& source, Iterator position) {
		const void* wanted = CheckpointContainerDetail::Node(position);
		const auto end = CheckpointContainerDetail::End(source);
		if (wanted == CheckpointNativeStorage::Original(CheckpointContainerDetail::Node(end))) return source.size();
		const auto* value = CheckpointNativeStorage::Original(CheckpointContainerDetail::Value(end, wanted));
		const auto values = CheckpointValues(source);
		for (size_t index = 0; index < values.size(); ++index) if (CheckpointNativeStorage::Original(&values[index]) == value) return index;
		throw std::logic_error("frozen list iterator lies outside its container");
	}

	template<class T, class A, class Iterator> size_t CheckpointIteratorIndex(const std::deque<T, A>& source, Iterator position) {
#ifdef _MSVC_STL_VERSION
		const auto first = source._Unchecked_begin();
		return position._Myoff - first._Myoff;
#elif defined(_LIBCPP_VERSION)
		if (!CheckpointNativeStorage::IsView(&source)) return static_cast<size_t>(typename std::deque<T, A>::const_iterator(position) - source.begin());
		struct Fields { const T* const* map; const T* value; };
		static_assert(sizeof(Iterator) == sizeof(Fields));
		const auto fields = std::bit_cast<Fields>(position);
		const auto header = CheckpointContainerDetail::Deque(source);
		if (!header.size) return 0;
		const size_t blockSize = std::__deque_block_size<T, ptrdiff_t>::value;
		const T* block = *CheckpointContainerDetail::Read(fields.map);
		return (fields.map - header.begin) * blockSize + (fields.value - block) - header.start;
#else
		return static_cast<size_t>(typename std::deque<T, A>::const_iterator(position) - source.begin());
#endif
	}
}
