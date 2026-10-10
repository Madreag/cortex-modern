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
#include <memory_resource>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct BITMAP;

namespace RTE {
	struct UnsupportedCheckpointNative : std::runtime_error { using std::runtime_error::runtime_error; };
	struct BitmapSnapshot;
	struct SoundData;
	class SoundSet;
	struct HitData;
	class Material;
	class MovableObject;

	/// Times what a boundary freeze spends per kind of value; off unless CCCP_CHECKPOINT_PHASES=1.
	class CheckpointCloneCost {
	public:
		explicit CheckpointCloneCost(const char* kind) : m_Kind(kind && Enabled() ? kind : nullptr) { if (m_Kind) Begin(); }
		~CheckpointCloneCost() { if (m_Kind) End(); }
		CheckpointCloneCost(const CheckpointCloneCost&) = delete;
		CheckpointCloneCost& operator=(const CheckpointCloneCost&) = delete;
		using Totals = std::vector<std::pair<const char*, std::array<int64_t, 3>>>;
		static bool Enabled();
		/// Takes the costs gathered so far, as count, inclusive and exclusive nanoseconds per kind.
		static Totals Take();
		/// Prints taken costs, the largest exclusive cost first.
		static void Report(uint64_t tick, Totals totals);
	private:
		void Begin();
		void End();
		const char* m_Kind;
		int64_t m_Start = 0, m_Children = 0;
		CheckpointCloneCost* m_Parent = nullptr;
	};

	/// A map the freezing threads share; each key's shard has its own lock, so threads freezing different objects rarely meet.
	template<class Key, class Value> class CheckpointSharedMap {
	public:
		std::optional<Value> Find(const Key& key) const {
			const Shard& shard = For(key);
			std::lock_guard lock(shard.mutex);
			const auto found = shard.map.find(key);
			if (found == shard.map.end()) return std::nullopt;
			return found->second;
		}
		/// The stored value's address, which stays put until its key is erased.
		const Value* FindStored(const Key& key) const {
			const Shard& shard = For(key);
			std::lock_guard lock(shard.mutex);
			const auto found = shard.map.find(key);
			return found == shard.map.end() ? nullptr : &found->second;
		}
		/// The value the key holds after this call, and whether this call put it there.
		std::pair<Value, bool> TryEmplace(const Key& key, Value value) {
			Shard& shard = For(key);
			std::lock_guard lock(shard.mutex);
			const auto [found, inserted] = shard.map.try_emplace(key, std::move(value));
			return {found->second, inserted};
		}
		void InsertOrAssign(const Key& key, Value value) {
			Shard& shard = For(key);
			std::lock_guard lock(shard.mutex);
			shard.map.insert_or_assign(key, std::move(value));
		}
		void Erase(const Key& key) {
			Shard& shard = For(key);
			std::lock_guard lock(shard.mutex);
			shard.map.erase(key);
		}
		size_t Size() const {
			size_t size = 0;
			for (const Shard& shard: m_Shards) {
				std::lock_guard lock(shard.mutex);
				size += shard.map.size();
			}
			return size;
		}
		template<class Visit> void ForEach(Visit visit) const {
			for (const Shard& shard: m_Shards) {
				std::lock_guard lock(shard.mutex);
				for (const auto& [key, value]: shard.map) visit(key, value);
			}
		}

	private:
		static constexpr size_t c_ShardBits = 6;
		struct alignas(64) Shard {
			mutable std::mutex mutex;
			// Entries live as long as the snapshot, so the shard takes their storage from its own growing buffer.
			std::pmr::monotonic_buffer_resource memory{1024};
			std::pmr::unordered_map<Key, Value> map{&memory};
		};
		static size_t ShardOf(const Key& key) { return static_cast<size_t>((static_cast<uint64_t>(std::hash<Key>{}(key)) * 0x9E3779B97F4A7C15ULL) >> (64 - c_ShardBits)); }
		const Shard& For(const Key& key) const { return m_Shards[ShardOf(key)]; }
		Shard& For(const Key& key) { return m_Shards[ShardOf(key)]; }
		std::array<Shard, size_t{1} << c_ShardBits> m_Shards;
	};

	// Snapshot constructors own archived fields without gameplay creation or callbacks; several threads may freeze at once.
	class CheckpointNativeSnapshot {
	public:
		CheckpointNativeSnapshot();
		~CheckpointNativeSnapshot();
		CheckpointNativeSnapshot(const CheckpointNativeSnapshot&) = delete;
		CheckpointNativeSnapshot& operator=(const CheckpointNativeSnapshot&) = delete;
		class BoundaryScope {
		public:
			explicit BoundaryScope(std::shared_ptr<CheckpointNativeSnapshot> snapshot) : m_Previous(std::move(s_Boundary)) { s_Boundary = std::move(snapshot); }
			~BoundaryScope() { s_Boundary = std::move(m_Previous); }
		private:
			std::shared_ptr<CheckpointNativeSnapshot> m_Previous;
		};
		static const std::shared_ptr<CheckpointNativeSnapshot>& Boundary() { return s_Boundary; }
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
		/// The frozen copy of a container's sound set, shared by every container whose set freezes to an equal copy.
		std::shared_ptr<SoundSet> FreezeSoundSet(const std::shared_ptr<SoundSet>& source);
		HitData Freeze(const HitData& source);
		void RememberMaterial(const Material* source, const Material* target);
		const CheckpointText* MaterialReference(const Material* target) const;
		/// The preset an entity was copied from, looked up once per name for the whole capture.
		const Entity* PresetFor(const Entity& source);
		void RememberUID(const MovableObject* source, MovableObject* target);
		MovableObject* FindUID(long uid) const;

		template<class T> T* Object(const T* source) {
			static_assert(std::is_base_of_v<Entity, std::remove_const_t<T>>);
			if (!source) return nullptr;
			const Entity* base = source;
			const ptrdiff_t offset = reinterpret_cast<const char*>(source) - reinterpret_cast<const char*>(base);
			auto& slot = Recent(base);
			Entity* target = static_cast<Entity*>(slot.second);
			if (slot.first != base) {
				if (const auto known = m_Objects.Find(base)) target = *known;
				else {
					thread_local bool insidePreset = false;
					const bool preset = CheckpointCloneCost::Enabled() && !insidePreset && base->IsOriginalPreset();
					CheckpointCloneCost presets(preset ? "preset clones (inclusive)" : nullptr);
					if (preset) insidePreset = true;
					struct Leave { bool active; ~Leave() { if (active) insidePreset = false; } } leave{preset};
					CheckpointCloneCost cost(CheckpointCloneCost::Enabled() ? base->GetClassName().c_str() : nullptr);
					target = source->FreezeCheckpointNative(*this);
				}
				slot = {base, target};
			}
			return reinterpret_cast<T*>(reinterpret_cast<char*>(target) + offset);
		}
		template<class T> T* ValueObject(const T* source) {
			if (!source) return nullptr;
			CheckpointFailure::Check(CheckpointFailure::Point::NativeObjects);
			void* memory = AllocateFrozen(sizeof(T), alignof(T));
			// Snapshot storage is cheap to abandon, so a value is claimed without looking for it first.
			if (!memory) if (const auto known = m_Values.Find(source)) return static_cast<T*>(*known);
			CheckpointCloneCost cost(CheckpointCloneCost::Enabled() ? typeid(T).name() : nullptr);
			const bool frozenStorage = memory != nullptr;
			void (*destroy)(void*) noexcept = [](void* value) noexcept { static_cast<T*>(value)->~T(); ::operator delete(value); };
			if (frozenStorage) destroy = [](void* value) noexcept { static_cast<T*>(value)->~T(); };
			auto& owner = AddValueOwner(destroy);
			if (!frozenStorage) memory = ::operator new(sizeof(T));
			const auto release = [frozenStorage, memory] { if (!frozenStorage) ::operator delete(memory); };
			std::pair<void*, bool> claim;
			try {
				claim = m_Values.TryEmplace(source, memory);
			} catch (...) {
				release();
				throw;
			}
			if (!claim.second) {
				release();
				return static_cast<T*>(claim.first);
			}
			try {
				if constexpr (requires { T::PrepareCheckpointNative(*source, static_cast<T*>(memory), *this); })
					T::PrepareCheckpointNative(*source, static_cast<T*>(memory), *this);
				::new(memory) T(*source, *this);
				owner.first = memory;
				return static_cast<T*>(memory);
			} catch (...) {
				m_Values.Erase(source);
				release();
				throw;
			}
		}

		/// Claims a top-level object's frozen storage before any thread freezes it: a reference from another object then
		/// names it without freezing it there, and the thread given the object freezes it with Construct.
		void Reserve(const Entity& source) {
			auto& type = const_cast<Entity::ClassInfo&>(source.GetClass());
			Entity** slot = AddOwner();
			void* memory = type.AllocateCheckpointMemory();
			Entity* target = reinterpret_cast<Entity*>(static_cast<char*>(memory) + (reinterpret_cast<const char*>(&source) - static_cast<const char*>(dynamic_cast<const void*>(&source))));
			try {
				if (!m_Objects.TryEmplace(&source, target).second) {
					type.DeallocateCheckpointMemory(memory);
					return;
				}
				m_Reserved.InsertOrAssign(&source, Reservation{memory, slot, &type});
			} catch (...) {
				m_Objects.Erase(&source);
				type.DeallocateCheckpointMemory(memory);
				throw;
			}
		}
		/// Freezes a reserved object on this thread.
		void Construct(const Entity& source) {
			if (m_Reserved.Find(&source)) {
				CheckpointCloneCost cost(CheckpointCloneCost::Enabled() ? source.GetClassName().c_str() : nullptr);
				const Entity* previous = std::exchange(t_Constructing, &source);
				struct Restore { const Entity* previous; ~Restore() { t_Constructing = previous; } } restore{previous};
				source.FreezeCheckpointNative(*this);
			}
		}

		template<class T> Entity* Make(const T& source) {
			CheckpointFailure::Check(CheckpointFailure::Point::NativeObjects);
			const ptrdiff_t offset = reinterpret_cast<const char*>(static_cast<const Entity*>(&source)) - reinterpret_cast<const char*>(&source);
			Entity** slot = nullptr;
			void* memory = nullptr;
			Entity* target = nullptr;
			bool frozenStorage = false;
			// Only Construct freezes a reserved object, and it names the one it freezes.
			if (const auto reserved = t_Constructing == static_cast<const Entity*>(&source) ? m_Reserved.Find(&source) : std::nullopt) {
				m_Reserved.Erase(&source);
				memory = reserved->memory;
				slot = reserved->slot;
				target = reinterpret_cast<Entity*>(static_cast<char*>(memory) + offset);
			} else {
				slot = AddOwner();
				memory = AllocateFrozen(sizeof(T), alignof(T));
				frozenStorage = memory != nullptr;
				if (!frozenStorage) memory = const_cast<Entity::ClassInfo&>(source.GetClass()).AllocateCheckpointMemory();
				if (!memory) throw std::bad_alloc();
				target = reinterpret_cast<Entity*>(static_cast<char*>(memory) + offset);
				// Two threads can reach one object through different owners; the first to claim it freezes it.
				std::pair<Entity*, bool> claim;
				try {
					claim = m_Objects.TryEmplace(&source, target);
				} catch (...) {
					if (!frozenStorage) T::Deallocate(memory);
					throw;
				}
				if (!claim.second) {
					if (!frozenStorage) T::Deallocate(memory);
					return claim.first;
				}
			}
			bool constructed = false;
			try {
				*slot = target;
				if constexpr (requires { T::PrepareCheckpointNative(source, reinterpret_cast<T*>(memory), *this); })
					T::PrepareCheckpointNative(source, reinterpret_cast<T*>(memory), *this);
				// The entity binds itself first thing; it is told its own slot instead of looking it up.
				const Binding previous = std::exchange(t_Binding, Binding{static_cast<const Entity*>(&source), target, slot});
				struct Restore { Binding previous; ~Restore() { t_Binding = previous; } } restore{previous};
				new(memory) T(source, *this);
				constructed = true;
				target->m_CheckpointAllocation = frozenStorage ? FrozenStorageMark(memory) : memory;
				return target;
			} catch (...) {
				if (constructed) std::launder(reinterpret_cast<T*>(memory))->~T();
				*slot = nullptr;
				m_Objects.Erase(&source);
				if (Entity::s_DeletedCheckpointMemory == memory || Entity::s_DeletedCheckpointMemory == FrozenStorageMark(memory)) Entity::s_DeletedCheckpointMemory = nullptr;
				if (!frozenStorage) T::Deallocate(memory);
				throw;
			}
		}

		/// Storage for one frozen object that lives until the snapshot ends, or null when the object is too large for it.
		void* AllocateFrozen(size_t bytes, size_t alignment);
		/// What a frozen object built in snapshot storage records as its allocation; deleting it then frees nothing.
		static void* FrozenStorageMark(void* memory) { return static_cast<char*>(memory) + 1; }

		Entity** Bind(const Entity& source, Entity* target) {
			if (t_Binding.source == &source && t_Binding.target == target) return t_Binding.slot;
			// Only an entity Make builds owns a slot; one built in place inside its owner has none.
			m_Objects.InsertOrAssign(&source, target);
			return nullptr;
		}
		template<class T> void BindValue(const T& source, T* target) { m_Values.InsertOrAssign(&source, target); }
		template<class T> T* CopyValue(const T* source) {
			if (!source) return nullptr;
			if (const auto known = m_Values.Find(source)) return static_cast<T*>(*known);
			auto& owner = AddValueOwner([](void* value) noexcept { delete static_cast<T*>(value); });
			auto value = std::make_unique<T>(Freeze(*source));
			const auto [winner, claimed] = m_Values.TryEmplace(source, value.get());
			if (!claimed) return static_cast<T*>(winner);
			owner.first = value.release();
			return static_cast<T*>(owner.first);
		}
		void AssignEntity(Entity& target, const Entity& source);
		template<class T> requires (!std::is_array_v<T>) void Prepare(const T& source, T* target) {
			if constexpr (std::is_base_of_v<Entity, T>) m_Objects.InsertOrAssign(&source, target);
			else m_Values.InsertOrAssign(&source, target);
			if constexpr (requires { T::PrepareCheckpointNative(source, target, *this); }) T::PrepareCheckpointNative(source, target, *this);
		}
		template<class T, size_t Size> void Prepare(const T (&source)[Size], T (*target)[Size]) {
			for (size_t index = 0; index < Size; ++index) Prepare(source[index], &(*target)[index]);
		}
		const Entity* Find(const Entity* source) const { return m_Objects.Find(source).value_or(nullptr); }
		size_t ObjectCount() const { return m_Objects.Size(); }

		template<class T> auto Freeze(const T& source) {
			if constexpr (requires { FreezeCheckpointValue(source, *this); }) return FreezeCheckpointValue(source, *this);
			else if constexpr (std::is_base_of_v<Entity, T> || requires { T(source, *this); }) return T(source, *this);
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
			if constexpr (requires(T& target, const T& value) { AssignCheckpointValue(target, value, *this); }) {
				for (const auto& value: source) { result.emplace_back(); AssignCheckpointValue(result.back(), value, *this); }
			} else if constexpr (requires(const T& value) { T(value, *this); }) {
				for (size_t index = 0; index < source.size(); ++index) Prepare(source[index], result.data() + index);
				for (const auto& value: source) result.emplace_back(value, *this);
			} else for (const auto& value: source) result.push_back(Freeze(static_cast<const T&>(value)));
			return result;
		}
		template<class T, class Allocator> auto Freeze(const std::deque<T, Allocator>& source) {
			std::deque<T, Allocator> result(source.get_allocator());
			if constexpr (requires(T& target, const T& value) { AssignCheckpointValue(target, value, *this); })
				for (const auto& value: source) { result.emplace_back(); AssignCheckpointValue(result.back(), value, *this); }
			else if constexpr (requires(const T& value) { T(value, *this); }) for (const auto& value: source) result.emplace_back(value, *this);
			else for (const auto& value: source) result.push_back(Freeze(value));
			return result;
		}
		template<class T, class Allocator> auto Freeze(const std::list<T, Allocator>& source) {
			std::list<T, Allocator> result(source.get_allocator());
			if constexpr (requires(T& target, const T& value) { AssignCheckpointValue(target, value, *this); })
				for (const auto& value: source) { result.emplace_back(); AssignCheckpointValue(result.back(), value, *this); }
			else if constexpr (requires(const T& value) { T(value, *this); }) for (const auto& value: source) result.emplace_back(value, *this);
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
		using ValueOwner = std::pair<void*, void (*)(void*) noexcept>;
		/// This thread's recent answers of this snapshot, read without a shard lock: the shared objects many owners
		/// name (materials, presets, sprites) would otherwise keep every thread on the same few locks.
		template<int Kind = 0> std::pair<const void*, void*>& Recent(const void* key) {
			struct Cache {
				uint64_t snapshot = 0;
				std::array<std::pair<const void*, void*>, 1024> slots{};
			};
			thread_local Cache cache;
			if (cache.snapshot != m_Serial) {
				cache.slots.fill({});
				cache.snapshot = m_Serial;
			}
			return cache.slots[(reinterpret_cast<uintptr_t>(key) >> 4) % cache.slots.size()];
		}
		const uint64_t m_Serial;
		/// The owner lists of the freezing thread; a slot keeps its address while other threads add theirs.
		struct alignas(64) OwnerShard {
			std::mutex mutex;
			std::deque<Entity*> owners;
			std::deque<ValueOwner> values;
			std::vector<std::shared_ptr<void>> storage;
		};
		OwnerShard& Owners() { return m_OwnerShards[std::hash<std::thread::id>{}(std::this_thread::get_id()) % m_OwnerShards.size()]; }
		Entity** AddOwner() {
			OwnerShard& shard = Owners();
			std::lock_guard lock(shard.mutex);
			shard.owners.push_back(nullptr);
			return &shard.owners.back();
		}
		ValueOwner& AddValueOwner(void (*destroy)(void*) noexcept) {
			OwnerShard& shard = Owners();
			std::lock_guard lock(shard.mutex);
			shard.values.push_back({nullptr, destroy});
			return shard.values.back();
		}
		inline static thread_local const CheckpointNativeSnapshot* s_Current = nullptr;
		inline static thread_local const Entity* t_Constructing = nullptr;
		struct Binding { const Entity* source = nullptr; Entity* target = nullptr; Entity** slot = nullptr; };
		inline static thread_local Binding t_Binding;
		inline static thread_local std::shared_ptr<CheckpointNativeSnapshot> s_Boundary;
		CheckpointSharedMap<const BITMAP*, std::shared_ptr<Pixel>> m_Bitmaps;
		CheckpointSharedMap<const BITMAP*, std::shared_ptr<Pixel>> m_BitmapSources;
		CheckpointSharedMap<const Serializable*, CheckpointText> m_WriterValues;
		CheckpointSharedMap<const Material*, CheckpointText> m_MaterialReferences;
		std::unordered_map<const Material*, std::pair<int, size_t>> m_MaterialOwners;
		CheckpointSharedMap<long, MovableObject*> m_UIDs;
		CheckpointFrozenClock m_Clock;
		struct SoundSetKeyHash { size_t operator()(const std::vector<uint64_t>& key) const noexcept; };
		struct SoundSetShard { std::mutex mutex; std::unordered_map<std::vector<uint64_t>, SoundSet*, SoundSetKeyHash> sets; };
		std::array<SoundSetShard, 16> m_SoundSets;
		struct PresetKey { const std::string* type; int module; std::string name; };
		struct PresetName { const std::string* type; int module; std::string_view name; };
		struct PresetHash {
			using is_transparent = void;
			size_t operator()(const PresetName& key) const noexcept;
			size_t operator()(const PresetKey& key) const noexcept { return (*this)(PresetName{key.type, key.module, key.name}); }
		};
		struct PresetEqual {
			using is_transparent = void;
			static PresetName View(const PresetKey& key) { return {key.type, key.module, key.name}; }
			static const PresetName& View(const PresetName& key) { return key; }
			template<class A, class B> bool operator()(const A& left, const B& right) const {
				const PresetName& a = View(left); const PresetName& b = View(right);
				return a.type == b.type && a.module == b.module && a.name == b.name;
			}
		};
		struct PresetShard { std::mutex mutex; std::unordered_map<PresetKey, const Entity*, PresetHash, PresetEqual> presets; };
		std::array<PresetShard, 16> m_Presets;
		std::array<OwnerShard, 32> m_OwnerShards;
		CheckpointSharedMap<const Entity*, Entity*> m_Objects;
		CheckpointSharedMap<const void*, void*> m_Values;
		struct Reservation {
			void* memory;
			Entity** slot;
			Entity::ClassInfo* type;
		};
		CheckpointSharedMap<const Entity*, Reservation> m_Reserved;

	};

}
