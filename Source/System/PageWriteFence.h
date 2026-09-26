#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RTE {

	/// Copy-on-write for a few large buffers while a preview writes them. The whole pages of each buffer are made
	/// read-only; the first write to a page copies it aside before it lands, and Restore puts back exactly the pages
	/// written. The partial pages at a buffer's ends are copied at Arm. Where the platform has no page fence, Arm
	/// refuses and the caller copies the buffers itself.
	class PageWriteFence {
	public:
		struct Buffer {
			uint8_t* data = nullptr;
			size_t bytes = 0;
		};

		/// Fences the buffers. False fences nothing.
		static bool Arm(const std::vector<Buffer>& buffers);
		/// True while armed over exactly these buffers, in this order.
		static bool Covers(const std::vector<Buffer>& buffers);
		/// Puts back every byte written since Arm and lifts the fence. Returns the pages put back.
		static size_t Restore();
		/// Lifts the fence and keeps what was written.
		static void Release();
		static bool IsArmed();
		/// False where the platform has no page fence yet; Arm always refuses there.
		static bool IsSupported();

		/// Pages first written under a fence since the process started.
		static uint64_t GetFaultCount();

		/// Empty when the fence puts back exactly what was written, else the first mismatch; for -cow-checkpoint-selftest.
		static std::string SelfTestMismatch();
	};
} // namespace RTE
