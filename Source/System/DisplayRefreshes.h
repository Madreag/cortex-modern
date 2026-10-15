#pragma once

#include <cstdint>

namespace RTE {

	/// The compositor's count of the display's refreshes, read without waiting on a swap; -1 where the platform keeps no such count.
	int64_t CompositorRefreshCount();
} // namespace RTE
