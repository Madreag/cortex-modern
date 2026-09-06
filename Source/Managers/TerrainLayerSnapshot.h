#pragma once

#include <cstdint>
#include <vector>

struct BITMAP;

namespace RTE {

	/// Raw copies of the three 8-bit terrain layers, restored by memcpy.
	struct TerrainLayerSnapshot {
		std::vector<uint8_t> mat;
		std::vector<uint8_t> fg;
		std::vector<uint8_t> bg;

		static void CopyFrom(BITMAP* bitmap, std::vector<uint8_t>& out);
		static bool CopyTo(BITMAP* bitmap, const std::vector<uint8_t>& in);

		bool Capture();
		bool Restore() const;
	};
} // namespace RTE
