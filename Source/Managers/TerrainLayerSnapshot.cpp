#include "TerrainLayerSnapshot.h"

#include "SceneMan.h"
#include "Scene.h"
#include "SLTerrain.h"
#include "CheckpointArchive.h"

#include "allegro.h"

#include <cstring>

namespace RTE {

	void TerrainLayerSnapshot::CopyFrom(BITMAP* bitmap, std::vector<uint8_t>& out) {
		if (!bitmap) { out.clear(); return; }
		out.resize(static_cast<size_t>(bitmap->w) * bitmap->h);
		for (int y = 0; y < bitmap->h; ++y) {
			std::memcpy(out.data() + static_cast<size_t>(y) * bitmap->w, bitmap->line[y], bitmap->w);
		}
	}

	bool TerrainLayerSnapshot::CopyTo(BITMAP* bitmap, const std::vector<uint8_t>& in) {
		if (!bitmap || bitmap_color_depth(bitmap) != 8 || static_cast<size_t>(bitmap->w) * bitmap->h != in.size()) {
			return false;
		}
		for (int y = 0; y < bitmap->h; ++y) {
			std::memcpy(bitmap->line[y], in.data() + static_cast<size_t>(y) * bitmap->w, bitmap->w);
		}
		return true;
	}

	bool TerrainLayerSnapshot::Capture() {
		Scene* scene = g_SceneMan.GetScene();
		SLTerrain* terrain = scene ? scene->GetTerrain() : nullptr;
		if (!terrain) {
			return false;
		}
		CopyFrom(terrain->GetMaterialBitmap(), mat);
		CopyFrom(terrain->GetFGColorBitmap(), fg);
		CopyFrom(terrain->GetBGColorBitmap(), bg);
		width = terrain->GetMaterialBitmap()->w;
		height = terrain->GetMaterialBitmap()->h;
		CopyFrom(terrain->m_MaterialCopy, materialCopy);
		CaptureLayer(terrain, terrainLayers[0], false);
		CaptureLayer(terrain->GetFGSceneLayer(), terrainLayers[1], false);
		CaptureLayer(terrain->GetBGSceneLayer(), terrainLayers[2], false);
		updatedMaterialAreas = terrain->m_UpdatedMaterialAreas;
		orbitDirection = static_cast<int>(terrain->m_OrbitDirection);
		layerToDraw = static_cast<int>(terrain->m_LayerToDraw);
		for (int team = 0; team < Activity::MaxTeamCount; ++team) {
			CaptureLayer(scene->m_apUnseenLayer[team], unseen[team], true);
			seenPixels[team] = scene->m_SeenPixels[team];
			cleanedPixels[team] = scene->m_CleanedPixels[team];
			unseenPixelSize[team] = scene->m_UnseenPixelSize[team];
			scanScheduled[team] = scene->m_ScanScheduled[team];
		}
		return true;
	}

	bool TerrainLayerSnapshot::CanRestore() const {
		SLTerrain* terrain = g_SceneMan.GetScene() ? g_SceneMan.GetScene()->GetTerrain() : nullptr;
		if (!terrain || width <= 0 || height <= 0) return false;
		const size_t size = static_cast<size_t>(width) * height;
		const auto compatible = [this](BITMAP* bitmap) { return bitmap && bitmap_color_depth(bitmap) == 8 && bitmap->w == width && bitmap->h == height; };
		if (!compatible(terrain->GetMaterialBitmap()) || !compatible(terrain->GetFGColorBitmap()) || !compatible(terrain->GetBGColorBitmap()) || mat.size() != size || fg.size() != size || bg.size() != size || (!materialCopy.empty() && materialCopy.size() != size)) return false;
		for (const Layer& layer: unseen) if (layer.width < 0 || layer.height < 0 || layer.pixels.size() != static_cast<size_t>(layer.width) * layer.height) return false;
		return true;
	}

	bool TerrainLayerSnapshot::Restore() const {
		if (!CanRestore()) return false;
		Scene* scene = g_SceneMan.GetScene();
		SLTerrain* terrain = scene->GetTerrain();
		// Construct changed fog layers before changing any live pixels.
		std::array<std::unique_ptr<SceneLayer>, 4> replacements;
		for (int team = 0; team < Activity::MaxTeamCount; ++team) {
			const Layer& saved = unseen[team];
			SceneLayer* live = scene->m_apUnseenLayer[team];
			BITMAP* bitmap = live ? live->GetBitmap() : nullptr;
			if (saved.width && (!bitmap || bitmap->w != saved.width || bitmap->h != saved.height)) {
				BITMAP* pixels = create_bitmap_ex(8, saved.width, saved.height);
				if (!pixels) return false;
				replacements[team] = std::make_unique<SceneLayer>();
				if (replacements[team]->Create(pixels, saved.masked, saved.offset, saved.wrapX, saved.wrapY, saved.scrollInfo) < 0) return false;
			}
		}
		CopyTo(terrain->GetMaterialBitmap(), mat);
		CopyTo(terrain->GetFGColorBitmap(), fg);
		CopyTo(terrain->GetBGColorBitmap(), bg);
		if (!materialCopy.empty()) { terrain->UpdateMaterialCopy(); CopyTo(terrain->m_MaterialCopy, materialCopy); }
		else if (terrain->m_MaterialCopy) { destroy_bitmap(terrain->m_MaterialCopy); terrain->m_MaterialCopy = nullptr; }
		RestoreLayer(terrain, terrainLayers[0]);
		RestoreLayer(terrain->GetFGSceneLayer(), terrainLayers[1]);
		RestoreLayer(terrain->GetBGSceneLayer(), terrainLayers[2]);
		terrain->m_UpdatedMaterialAreas = updatedMaterialAreas;
		terrain->m_OrbitDirection = static_cast<Directions>(orbitDirection);
		terrain->m_LayerToDraw = static_cast<SLTerrain::LayerType>(layerToDraw);
		for (int team = 0; team < Activity::MaxTeamCount; ++team) {
			if (!unseen[team].width || replacements[team]) {
				delete scene->m_apUnseenLayer[team];
				scene->m_apUnseenLayer[team] = replacements[team].release();
			}
			if (SceneLayer* live = scene->m_apUnseenLayer[team]) { CopyTo(live->GetBitmap(), unseen[team].pixels); RestoreLayer(live, unseen[team]); }
			scene->m_SeenPixels[team] = seenPixels[team];
			scene->m_CleanedPixels[team] = cleanedPixels[team];
			scene->m_UnseenPixelSize[team] = unseenPixelSize[team];
			scene->m_ScanScheduled[team] = scanScheduled[team];
		}
		return true;
	}

	void TerrainLayerSnapshot::CaptureLayer(SceneLayer* source, Layer& target, bool pixels) {
		if (!source) { target = {}; return; }
		BITMAP* bitmap = source->GetBitmap();
		target.width = bitmap ? bitmap->w : 0; target.height = bitmap ? bitmap->h : 0;
		if (pixels) CopyFrom(bitmap, target.pixels);
		target.masked = source->m_DrawMasked; target.wrapX = source->m_WrapX; target.wrapY = source->m_WrapY;
		target.origin = source->m_OriginOffset; target.offset = source->m_Offset; target.zOrder = source->m_ZOrder;
		target.scrollInfo = source->m_ScrollInfo; target.scrollRatio = source->m_ScrollRatio;
		target.scale = source->m_ScaleFactor; target.scaledDimensions = source->m_ScaledDimensions;
		target.entityState = source->Entity::SaveCheckpoint();
		target.contentState = source->m_BitmapFile.SaveCheckpoint();
	}

	void TerrainLayerSnapshot::RestoreLayer(SceneLayer* target, const Layer& source) {
		target->m_DrawMasked = source.masked; target->m_WrapX = source.wrapX; target->m_WrapY = source.wrapY;
		target->m_OriginOffset = source.origin; target->m_Offset = source.offset; target->m_ZOrder = source.zOrder;
		target->m_ScrollInfo = source.scrollInfo; target->m_ScrollRatio = source.scrollRatio;
		target->m_ScaleFactor = source.scale; target->m_ScaledDimensions = source.scaledDimensions;
		if (!source.entityState.empty() && !target->Entity::LoadCheckpoint(source.entityState)) throw std::runtime_error("invalid layer identity");
		if (!source.contentState.empty() && !target->m_BitmapFile.LoadCheckpoint(source.contentState)) throw std::runtime_error("invalid layer content metadata");
		target->SetUpdated();
	}

	std::string TerrainLayerSnapshot::Layer::SaveCheckpoint() const {
		CheckpointWriter writer("TerrainLayer1");
		writer(width, height, masked, wrapX, wrapY, origin, offset, scrollInfo, scrollRatio, scale, scaledDimensions, zOrder, entityState, contentState);
		return writer.Text();
	}

	bool TerrainLayerSnapshot::Layer::LoadCheckpoint(std::string_view text, bool validateOnly) {
		try {
			CheckpointReader reader(text, "TerrainLayer1", validateOnly);
			reader(width, height, masked, wrapX, wrapY, origin, offset, scrollInfo, scrollRatio, scale, scaledDimensions, zOrder, entityState, contentState);
			reader.Finish();
			return true;
		} catch (const std::exception&) { return false; }
	}

	std::string TerrainLayerSnapshot::SaveMetadata() const {
		CheckpointWriter writer("TerrainMetadata1");
		writer(width, height, terrainLayers, unseen, updatedMaterialAreas, orbitDirection, layerToDraw, seenPixels, cleanedPixels, unseenPixelSize, scanScheduled);
		writer(std::string(materialCopy.begin(), materialCopy.end()));
		return writer.Text();
	}

	bool TerrainLayerSnapshot::LoadMetadata(std::string_view text, bool validateOnly) {
		try {
			CheckpointReader reader(text, "TerrainMetadata1", validateOnly);
			reader(width, height, terrainLayers, unseen, updatedMaterialAreas, orbitDirection, layerToDraw, seenPixels, cleanedPixels, unseenPixelSize, scanScheduled);
			std::string copy;
			reader.Value(copy);
			reader.OnCommit([this, copy = std::move(copy)] { materialCopy.assign(copy.begin(), copy.end()); });
			reader.Finish();
			return true;
		} catch (const std::exception&) { return false; }
	}
} // namespace RTE
