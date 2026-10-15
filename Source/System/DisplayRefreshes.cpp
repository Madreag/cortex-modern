#include "DisplayRefreshes.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

namespace RTE {

	int64_t CompositorRefreshCount() {
#ifdef _WIN32
		DWM_TIMING_INFO info{};
		info.cbSize = sizeof(info);
		return SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &info)) ? static_cast<int64_t>(info.cRefresh) : -1;
#else
		return -1;
#endif
	}
} // namespace RTE
