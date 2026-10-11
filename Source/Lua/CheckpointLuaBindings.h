#pragma once

#include "CheckpointFrozenContainers.h"
#include "CheckpointLuaHeap.h"
#include "CheckpointNativeStorage.h"
#include "CheckpointString.h"

#include <luabind/detail/class_rep.hpp>

#include <mutex>
#include <stdexcept>

namespace RTE::CheckpointLua {
	// Class names and base lists outlive Lua collection through the same owned pages as native values.
	struct BindingMetadata : CheckpointNativeAllocated {
		CheckpointString name;
		CheckpointVector<luabind::detail::class_rep::base_info> bases;
		explicit BindingMetadata(const luabind::detail::class_rep& source) : name(source.name()), bases(source.bases().begin(), source.bases().end()) {}
		static void Install() {
			static std::once_flag installed;
			std::call_once(installed, [] {
				luabind::detail::checkpoint_class_metadata_delete = [](void* metadata) { delete static_cast<BindingMetadata*>(metadata); };
				luabind::detail::checkpoint_class_metadata = [](const luabind::detail::class_rep* source) -> void* {
					return CheckpointNativeStorage::Enabled() ? new BindingMetadata(*source) : nullptr;
				};
			});
		}
	};

	class FrozenBindings {
	public:
		explicit FrozenBindings(const Snapshot& heap) : m_Heap(heap) {}
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
		TValue Members(const luabind::detail::class_rep* source) const { return m_Heap.RegistryValue(Class(source)->checkpoint_table_reference()); }
		TValue Instance(const GCudata* source) const {
			const auto* object = Object(source);
			return m_Heap.RegistryValue(object ? object->get_lua_table().checkpoint_reference() : LUA_NOREF);
		}
	private:
		const Snapshot& m_Heap;
	};
}
