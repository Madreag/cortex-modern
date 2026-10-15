#include "DisplayRefreshes.h"

#ifdef _WIN32
// The project trims windows.h for every file; dwmapi.h needs the dialog and drawing types back, so this file builds on its own.
#undef NOGDI
#undef NOKERNEL
#undef NONLS
#undef NOMEMMGR
#undef NOGDICAPMASKS
#undef NOVIRTUALKEYCODES
#undef NOWINMESSAGES
#undef NOWINSTYLES
#undef NOMETAFILE
#undef NOSCROLL
#undef NOTEXTMETRIC
#undef NOCOMM
#undef NOKANJI
#undef NOHELP
#undef NOPROFILER
#undef NODEFERWINDOWPOS
#undef NOMCX
#undef NOMENUS
#undef NOICONS
#undef NOKEYSTATES
#undef NOSYSCOMMANDS
#undef NORASTEROPS
#undef NOSHOWWINDOW
#undef NOATOM
#undef NODRAWTEXT
#undef NOCOLOR
#undef NOCTLMGR
#undef NOMSG
#undef NOOPENFILE
#undef NOSERVICE
#undef NOSOUND
#undef NOWH
#undef NOWINOFFSETS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
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
