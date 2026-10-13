#pragma once

#include <cstdint>
#include <map>
#include <memory>

namespace RTE::CheckpointLua {
	struct BindingMetadata;
	// The prepared boundary keeps callbacks without pulling bindings into game manager builds.
	class BindingLease {
	public:
		static BindingLease Prepare();
		bool Current() const noexcept;
		bool Keeps(const void* callbacks) const { return m_Callbacks.contains(callbacks); }
	private:
		friend struct BindingMetadata;
		uint64_t m_Generation = 0;
		std::map<const void*, std::shared_ptr<const void>> m_Callbacks;
	};
}
