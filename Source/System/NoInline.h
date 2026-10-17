#pragma once

// Keeps a function out of its callers. MSVC does not inline a function that reads a thread-local, so a hot check whose rare
// path reads one is kept inlinable by keeping that path a call.
#ifdef _MSC_VER
#define RTE_NOINLINE __declspec(noinline)
#else
#define RTE_NOINLINE __attribute__((noinline))
#endif
