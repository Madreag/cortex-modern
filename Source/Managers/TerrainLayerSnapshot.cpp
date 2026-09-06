#include "TerrainLayerSnapshot.h"

#include "SceneMan.h"
#include "Scene.h"
#include "SLTerrain.h"

#include "allegro.h"

#include <cstring>

namespace RTE {

	void TerrainLayerSnapshot::CopyFrom(BITMAP* bitmap, std::vector<uint8_t>& out) {
		out.resize(static_cast<size_t>(bitmap->w) * bitmap->h);
		for (int y = 0; y < bitmap->h; ++y) {
			std::memcpy(out.data() + static_cast<size_t>(y) * bitmap->w, bitmap->line[y], bitmap->w);
		}
	}

	bool TerrainLayerSnapshot::CopyTo(BITMAP* bitmap, const std::vector<uint8_t>& in) {
		if (static_cast<size_t>(bitmap->w) * bitmap->h != in.size()) {
			return false;
		}
		for (int y = 0; y < bitmap->h; ++y) {
			std::memcpy(bitmap->line[y], in.data() + static_cast<size_t>(y) * bitmap->w, bitmap->w);
		}
		return true;
	}

	bool TerrainLayerSnapshot::Capture() {
		SLTerrain* terrain = g_SceneMan.GetScene() ? g_SceneMan.GetScene()->GetTerrain() : nullptr;
		if (!terrain) {
			return false;
		}
		CopyFrom(terrain->GetMaterialBitmap(), mat);
		CopyFrom(terrain->GetFGColorBitmap(), fg);
		CopyFrom(terrain->GetBGColorBitmap(), bg);
		return true;
	}

	bool TerrainLayerSnapshot::Restore() const {
		SLTerrain* terrain = g_SceneMan.GetScene() ? g_SceneMan.GetScene()->GetTerrain() : nullptr;
		return terrain && CopyTo(terrain->GetMaterialBitmap(), mat) && CopyTo(terrain->GetFGColorBitmap(), fg) && CopyTo(terrain->GetBGColorBitmap(), bg);
	}
} // namespace RTE
