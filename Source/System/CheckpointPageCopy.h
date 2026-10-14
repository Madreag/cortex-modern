#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif

namespace RTE {

	/// Test lever CCCP_TEST_CHECKPOINT_PLAIN_COPY: a checkpoint's page copies go through the caches, as a plain memcpy does.
	inline const bool c_PlainCheckpointPageCopy = std::getenv("CCCP_TEST_CHECKPOINT_PLAIN_COPY") != nullptr;

	/// Copies whole pages into a checkpoint's own storage without pulling them through the caches: a save copies hundreds of
	/// megabytes the game never reads back, and through the caches that evicts the game's own working set. Callers publish
	/// the copied pages only after this returns; it is safe in a fault handler.
	inline void CopyCheckpointPages(void* destination, const void* source, size_t bytes) noexcept {
#if defined(_M_X64) || defined(__x86_64__)
		if (!c_PlainCheckpointPageCopy && ((reinterpret_cast<uintptr_t>(destination) | reinterpret_cast<uintptr_t>(source) | bytes) & 63) == 0) {
			auto* out = static_cast<__m128i*>(destination);
			const auto* in = static_cast<const __m128i*>(source);
			for (size_t index = 0; index < bytes / sizeof(__m128i); index += 4) {
				const __m128i first = _mm_load_si128(in + index), second = _mm_load_si128(in + index + 1);
				const __m128i third = _mm_load_si128(in + index + 2), fourth = _mm_load_si128(in + index + 3);
				_mm_stream_si128(out + index, first);
				_mm_stream_si128(out + index + 1, second);
				_mm_stream_si128(out + index + 2, third);
				_mm_stream_si128(out + index + 3, fourth);
			}
			// Streaming stores are weakly ordered: they are visible before the caller marks the pages saved.
			_mm_sfence();
			return;
		}
#endif
		std::memcpy(destination, source, bytes);
	}
} // namespace RTE
