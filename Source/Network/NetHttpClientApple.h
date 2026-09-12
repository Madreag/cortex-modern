#pragma once

#include <stddef.h>

// A C interface: the .mm is compiled by clang, whose C++ library can differ from the game compiler's.
#ifdef __cplusplus
extern "C" {
#endif

/// Called once with the outcome; statusCode is 0 and error is non-empty when the request failed.
typedef void (*NetHttpAppleDoneFn)(void* context, long statusCode, const char* body, size_t bodySize, const char* error);

/// Returns the running request, or null after reporting through done() a request that could not start.
void* NetHttpAppleStart(const char* method, const char* url, const char* const* headerNames, const char* const* headerValues, size_t headerCount, const char* body, size_t bodySize, const char* certPinSha256, int connectTimeoutMs, int totalTimeoutMs, NetHttpAppleDoneFn done, void* context);

/// Cancels and frees the request; done() never runs after this returns. False when done() never ran.
bool NetHttpAppleCancel(void* request, int waitMs);

#ifdef __cplusplus
}
#endif
