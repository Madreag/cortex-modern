#pragma once

// A C interface: the .mm is compiled by clang, whose C++ library can differ from the game compiler's.
#ifdef __cplusplus
extern "C" {
#endif

/// Registers the AppKit defaults the game needs before SDL creates NSApp; macOS only.
void AppleRegisterApplicationDefaults(void);

#ifdef __cplusplus
}
#endif
