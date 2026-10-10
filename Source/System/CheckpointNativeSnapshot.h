#pragma once

#include "Entity.h"
#include "CheckpointFailure.h"
#include "CheckpointFrozenClock.h"

#include <array>
#include <atomic>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct BITMAP;

namespace RTE {
	struct BitmapSnapshot;
	struct SoundData;
	struct HitData;
	class Material;
	class MovableObject;

	// Snapshot constructors own archived fields without gameplay creation or callbacks.
	class CheckpointNativeSnapshot {
	public:
		CheckpointNativeSnapshot();
		~CheckpointNativeSnapshot();
		CheckpointNativeSnapshot(const CheckpointNativeSnapshot&) = delete;
		CheckpointNativeSnapshot& operator=(const CheckpointNativeSnapshot&) = delete;
		class ReadScope {
		public:
			explicit ReadScope(const CheckpointNativeSnapshot* snapshot) : m_Previous(s_Current), m_Clock(snapshot ? &snapshot->m_Clock : nullptr) { if (snapshot) s_Current = snapshot; }
			~ReadScope() { s_Current = m_Previous; }
		private:
			const CheckpointNativeSnapshot* m_Previous;
			CheckpointFrozenClock::Scope m_Clock;
		};
		static const CheckpointNativeSnapshot* Current() { return s_Current; }
		BITMAP* Freeze(BITMAP* source);
		const BITMAP* Freeze(const BITMAP* source) { return Freeze(const_cast<BITMAP*>(source)); }
		std::optional<std::pair<std::shared_ptr<const BitmapSnapshot>, CheckpointText>> Pixels(const BITMAP* bitmap) const;
		std::optional<const std::string*> BitmapPath(const BITMAP* bitmap, int& depth) const;
		void MaterializePixels() const;
		CheckpointText FreezeWriter(const Serializable* source);
		SoundData Freeze(const SoundData& source);
		HitData Freeze(const HitData& source);
		void RememberMaterial(const Material* source, const Material* target);
		const CheckpointText* MaterialReference(const Material* target) const;
		void RememberUID(const MovableObject* source, MovableObject* target);
		MovableObject* FindUID(long uid) const;

		template<class T> T* Object(const T* source) {
			static_assert(std::is_base_of_v<Entity, std::remove_const_t<T>>);
			if (!source) return nullptr;
			const Entity* base = source;
			const ptrdiff_t offset = reinterpret_cast<const char*>(source) - reinterpret_cast<const char*>(base);
			Entity* target = nullptr;
			if (const auto known = m_Objects.find(base); known != m_Objects.end()) target = known->second;
			else target = source->FreezeCheckpointNative(*this);
			return reinterpret_cast<T*>(reinterpret_cast<char*>(target) + offset);
		}
		template<class T> T* ValueObject(const T* source) {
			if (!source) return nullptr;
			if (const auto known = m_Values.find(source); known != m_Values.end()) return static_cast<T*>(known->second);
			m_ValueOwners.push_back({nullptr, [](void* value) noexcept { static_cast<T*>(value)->~T(); ::operator delete(value); }});
			auto& owner = m_ValueOwners.back();
			CheckpointFailure::Check(CheckpointFailure::Point::NativeObjects);
			void* memory = ::operator new(sizeof(T));
			try {
				m_Values.emplace(source, memory);
				if constexpr (requires { T::PrepareCheckpointNative(*source, static_cast<T*>(memory), *this); })
					T::PrepareCheckpointNative(*source, static_cast<T*>(memory), *this);
				::new(memory) T(*source, *this);
				owner.first = memory;
				return static_cast<T*>(memory);
			} catch (...) {
				m_Values.erase(source);
				::operator delete(memory);
				throw;
			}
		}

		template<class T> Entity* Make(const T& source) {
			CheckpointFailure::Check(CheckpointFailure::Point::NativeObjects);
			m_Owners.push_back(nullptr);
			Entity** slot = &m_Owners.back();
			void* memory = const_cast<Entity::ClassInfo&>(source.GetClass()).GetCheckpointPoolMemory();
			if (!memory) throw std::bad_alloc();
			const ptrdiff_t offset = reinterpret_cast<const char*>(static_cast<const Entity*>(&source)) - reinterpret_cast<const char*>(&source);
			Entity* target = reinterpret_cast<Entity*>(static_cast<char*>(memory) + offset);
			bool constructed = false;
			try {
				m_Objects.emplace(&source, target);
				m_Slots.emplace(target, slot);
				*slot = target;
				if constexpr (requires { T::PrepareCheckpointNative(source, reinterpret_cast<T*>(memory), *this); })
					T::PrepareCheckpointNative(source, reinterpret_cast<T*>(memory), *this);
				new(memory) T(source, *this);
				constructed = true;
				return target;
			} catch (...) {
				if (constructed) std::launder(reinterpret_cast<T*>(memory))->~T();
				*slot = nullptr;
				m_Objects.erase(&source);
				m_Slots.erase(target);
				T::operator delete(memory);
				throw;
			}
		}

		Entity** Bind(const Entity& source, Entity* target) {
			m_Objects.insert_or_assign(&source, target);
			if (const auto slot = m_Slots.find(target); slot != m_Slots.end()) return slot->second;
			return nullptr;
		}
		template<class T> void BindValue(const T& source, T* target) { m_Values.insert_or_assign(&source, target); }
		template<class T> T* CopyValue(const T* source) {
			if (!source) return nullptr;
			if (const auto known = m_Values.find(source); known != m_Values.end()) return static_cast<T*>(known->second);
			m_ValueOwners.push_back({nullptr, [](void* value) noexcept { delete static_cast<T*>(value); }});
			auto& owner = m_ValueOwners.back();
			auto value = std::make_unique<T>(Freeze(*source));
			m_Values.emplace(source, value.get());
			owner.first = value.release();
			return static_cast<T*>(owner.first);
		}
		void AssignEntity(Entity& target, const Entity& source);
		template<class T> void Prepare(const T& source, T* target) {
			if constexpr (std::is_base_of_v<Entity, T>) m_Objects.insert_or_assign(&source, target);
			else m_Values.insert_or_assign(&source, target);
			if constexpr (requires { T::PrepareCheckpointNative(source, target, *this); }) T::PrepareCheckpointNative(source, target, *this);
		}
		template<class T, size_t Size> void Prepare(const T (&source)[Size], T (*target)[Size]) {
			for (size_t index = 0; index < Size; ++index) Prepare(source[index], &(*target)[index]);
		}
		const Entity* Find(const Entity* source) const {
			const auto found = m_Objects.find(source);
			return found == m_Objects.end() ? nullptr : found->second;
		}
		size_t ObjectCount() const { return m_Objects.size(); }

		template<class T> auto Freeze(const T& source) {
			if constexpr (std::is_base_of_v<Entity, T> || requires { T(source, *this); }) return T(source, *this);
			else { static_assert(std::is_copy_constructible_v<T>); return T(source); }
		}
		template<class T> T* Freeze(T* source) {
			if constexpr (std::is_base_of_v<Entity, std::remove_const_t<T>>) return Object(source);
			else if constexpr (requires { std::remove_const_t<T>(*source, *this); }) return ValueObject(source);
			else { static_assert(sizeof(T) == 0, "native pointer fields need an owned snapshot policy"); }
		}
		template<class T> auto Freeze(const std::atomic<T>& source) { return source.load(std::memory_order_acquire); }
		template<class A, class B> auto Freeze(const std::pair<A, B>& source) { return std::pair{Freeze(source.first), Freeze(source.second)}; }
		template<class T, size_t Size> auto Freeze(const std::array<T, Size>& source) {
			return [&]<size_t... Index>(std::index_sequence<Index...>) { return std::array<T, Size>{Freeze(source[Index])...}; }(std::make_index_sequence<Size>{});
		}
		template<class T, class Allocator> auto Freeze(const std::vector<T, Allocator>& source) {
			std::vector<T, Allocator> result(source.get_allocator());
			result.reserve(source.size());
			if constexpr (requires(const T& value) { T(value, *this); }) {
				for (size_t index = 0; index < source.size(); ++index) Prepare(source[index], result.data() + index);
				for (const auto& value: source) result.emplace_back(value, *this);
			} else for (const auto& value: source) result.push_back(Freeze(static_cast<const T&>(value)));
			return result;
		}
		template<class T, class Allocator> auto Freeze(const std::deque<T, Allocator>& source) {
			std::deque<T, Allocator> result(source.get_allocator());
			if constexpr (requires(const T& value) { T(value, *this); }) for (const auto& value: source) result.emplace_back(value, *this);
			else for (const auto& value: source) result.push_back(Freeze(value));
			return result;
		}
		template<class T, class Allocator> auto Freeze(const std::list<T, Allocator>& source) {
			std::list<T, Allocator> result(source.get_allocator());
			if constexpr (requires(const T& value) { T(value, *this); }) for (const auto& value: source) result.emplace_back(value, *this);
			else for (const auto& value: source) result.push_back(Freeze(value));
			return result;
		}
		template<class T, class Container> auto Freeze(const std::queue<T, Container>& source) {
			auto remaining = source;
			std::queue<T, Container> result;
			while (!remaining.empty()) { result.push(Freeze(remaining.front())); remaining.pop(); }
			return result;
		}
		template<class Key, class Value, class Compare, class Allocator> auto Freeze(const std::map<Key, Value, Compare, Allocator>& source) {
			std::map<Key, Value, Compare, Allocator> result(source.key_comp(), source.get_allocator());
			for (const auto& [key, value]: source) result.emplace(key, Freeze(value));
			return result;
		}
		template<class Key, class Value, class Hash, class Compare, class Allocator> auto Freeze(const std::unordered_map<Key, Value, Hash, Compare, Allocator>& source) {
			// Copying first preserves the source's iteration order and buckets.
			auto result = source;
			for (auto& [key, value]: result) value = Freeze(source.at(key));
			return result;
		}
		template<class Key, class Compare, class Allocator> auto Freeze(const std::set<Key, Compare, Allocator>& source) {
			static_assert(!std::is_pointer_v<Key>, "native pointer sets need a recorded order");
			return source;
		}
		template<class Key, class Hash, class Compare, class Allocator> auto Freeze(const std::unordered_set<Key, Hash, Compare, Allocator>& source) {
			static_assert(!std::is_pointer_v<Key>, "native pointer sets need a recorded order");
			return source;
		}
		template<class T, class Delete> auto Freeze(const std::unique_ptr<T, Delete>& source) {
			static_assert(std::is_same_v<Delete, std::default_delete<T>> && std::is_base_of_v<Entity, T>);
			return std::unique_ptr<T>(Object(source.get()));
		}
		template<class T> auto Freeze(const std::shared_ptr<T>& source) {
			if (!source) return std::shared_ptr<T>();
			if constexpr (std::is_base_of_v<Entity, T>) return std::shared_ptr<T>(Object(source.get()), [](T*) {});
			else return std::shared_ptr<T>(ValueObject(source.get()), [](T*) {});
		}
		template<class T> auto Freeze(const std::optional<T>& source) { return source ? std::optional<T>(Freeze(*source)) : std::optional<T>(); }
		template<class T, size_t Size> void FreezeArray(T (&target)[Size], const T (&source)[Size]) {
			for (size_t index = 0; index < Size; ++index) {
				if constexpr (std::is_array_v<T>) FreezeArray(target[index], source[index]);
				else if constexpr (requires { target[index].AssignCheckpointNative(source[index], *this); }) target[index].AssignCheckpointNative(source[index], *this);
				else target[index] = Freeze(source[index]);
			}
		}

	private:
		struct Pixel;
		inline static thread_local const CheckpointNativeSnapshot* s_Current = nullptr;
		std::unordered_map<const BITMAP*, std::shared_ptr<Pixel>> m_Bitmaps;
		std::unordered_map<const BITMAP*, std::shared_ptr<Pixel>> m_BitmapSources;
		std::unordered_map<const Serializable*, CheckpointText> m_WriterValues;
		std::unordered_map<const Material*, CheckpointText> m_MaterialReferences;
		std::unordered_map<long, MovableObject*> m_UIDs;
		CheckpointFrozenClock m_Clock;
		std::list<Entity*> m_Owners;
		std::unordered_map<const Entity*, Entity*> m_Objects;
		std::unordered_map<Entity*, Entity**> m_Slots;
		std::unordered_map<const void*, void*> m_Values;
		std::list<std::pair<void*, void (*)(void*) noexcept>> m_ValueOwners;
	};

}
