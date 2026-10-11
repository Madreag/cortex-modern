#pragma once

#include "CheckpointFrozenContainers.h"
#include "CheckpointLuaHeap.h"
#include "CheckpointNativeStorage.h"
#include "CheckpointString.h"
#include "CheckpointBindingLease.h"

#include <luabind/detail/class_rep.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace RTE::CheckpointLua {
	// Class names and base lists outlive Lua collection through the same owned pages as native values.
	struct BindingMetadata : CheckpointNativeAllocated {
		struct Callbacks {
			std::map<std::string, luabind::detail::class_rep::callback, std::less<>> getters;
			explicit Callbacks(const luabind::detail::class_rep& source) {
				for (const auto& [name, getter]: source.properties()) getters.emplace(name, getter);
			}
		};
		using Lease = BindingLease;
		static uint64_t Generation() noexcept { return RegistryGeneration().load(std::memory_order_acquire); }
		CheckpointString name;
		CheckpointVector<luabind::detail::class_rep::base_info> bases;
		const std::shared_ptr<const Callbacks> callbacks;
		explicit BindingMetadata(const luabind::detail::class_rep& source) : name(source.name()), bases(source.bases().begin(), source.bases().end()),
		    callbacks(std::make_shared<Callbacks>(source)) {
			auto& registry = Registry();
			std::lock_guard lock(registry.mutex);
			registry.values.emplace(callbacks.get(), callbacks);
			RegistryGeneration().fetch_add(1, std::memory_order_release);
		}
		~BindingMetadata() {
			auto& registry = Registry();
			std::lock_guard lock(registry.mutex);
			registry.values.erase(callbacks.get());
			RegistryGeneration().fetch_add(1, std::memory_order_release);
		}
		static Lease PrepareLease() {
			Lease result;
			auto& registry = Registry();
			std::lock_guard lock(registry.mutex);
			for (const auto& [identity, owner]: registry.values) if (auto callbacks = owner.lock()) result.m_Callbacks.emplace(identity, std::move(callbacks));
			result.m_Generation = RegistryGeneration().load(std::memory_order_acquire);
			return result;
		}
		static void Install() {
			static std::once_flag installed;
			std::call_once(installed, [] {
				luabind::detail::checkpoint_class_metadata_delete = [](void* metadata) { delete static_cast<BindingMetadata*>(metadata); };
				luabind::detail::checkpoint_class_metadata = [](const luabind::detail::class_rep* source) -> void* {
					return CheckpointNativeStorage::Enabled() ? new BindingMetadata(*source) : nullptr;
				};
			});
		}
	private:
		struct Owners {
			std::mutex mutex;
			std::map<const Callbacks*, std::weak_ptr<const Callbacks>> values;
		};
		static Owners& Registry() { static auto* owners = new Owners; return *owners; }
		static std::atomic<uint64_t>& RegistryGeneration() { static auto* generation = new std::atomic<uint64_t>{0}; return *generation; }
	};

	class FrozenBindings {
	public:
		explicit FrozenBindings(const Snapshot& heap, const BindingMetadata::Lease* callbacks = nullptr) : m_Heap(heap), m_Callbacks(callbacks) {}
		const luabind::detail::class_rep* Class(const luabind::detail::class_rep* source) const {
			if (!source) return nullptr;
			return reinterpret_cast<const luabind::detail::class_rep*>(m_Heap.ReadBytes(source, sizeof(*source)).data());
		}
		const luabind::detail::object_rep* Object(const GCudata* source) const {
			if (!source || m_Heap.Read(&source->len) < sizeof(luabind::detail::object_rep)) return nullptr;
			return reinterpret_cast<const luabind::detail::object_rep*>(m_Heap.ReadBytes(source + 1, sizeof(luabind::detail::object_rep)).data());
		}
		const BindingMetadata& Metadata(const luabind::detail::class_rep* source) const {
			const auto* type = Class(source);
			const auto* metadata = type ? static_cast<const BindingMetadata*>(type->checkpoint_metadata()) : nullptr;
			if (!metadata) throw std::runtime_error("a frozen Lua class has no owned metadata");
			return *CheckpointNativeStorage::Source(metadata);
		}
		bool DerivesFrom(const luabind::detail::class_rep* source, std::string_view name) const {
			const auto& metadata = Metadata(source);
			if (metadata.name == name) return true;
			for (const auto& base: CheckpointValues(metadata.bases)) if (base.base && DerivesFrom(base.base, name)) return true;
			return false;
		}
		std::optional<ptrdiff_t> BaseOffset(const luabind::detail::class_rep* source, std::string_view name) const {
			const auto& metadata = Metadata(source);
			if (metadata.name == name) return 0;
			for (const auto& base: CheckpointValues(metadata.bases)) {
				if (base.base) if (const auto offset = BaseOffset(base.base, name)) return base.pointer_offset + *offset;
			}
			return std::nullopt;
		}
		TValue Members(const luabind::detail::class_rep* source) const { return m_Heap.RegistryValue(Class(source)->checkpoint_table_reference()); }
		const luabind::detail::class_rep::callback* Getter(const luabind::detail::class_rep* source, std::string_view name) const {
			const auto* callbacks = Metadata(source).callbacks.get();
			if (!m_Callbacks || !m_Callbacks->Keeps(callbacks)) throw std::logic_error("a frozen Lua getter has no retained callback owner");
			const auto found = callbacks->getters.find(name);
			return found == callbacks->getters.end() ? nullptr : &found->second;
		}
		TValue Instance(const GCudata* source) const {
			const auto* object = Object(source);
			return m_Heap.RegistryValue(object ? object->get_lua_table().checkpoint_reference() : LUA_NOREF);
		}
	private:
		const Snapshot& m_Heap;
		const BindingMetadata::Lease* m_Callbacks;
	};
}
