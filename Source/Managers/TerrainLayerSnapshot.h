#pragma once

#include <cstdint>
#include <deque>
#include <array>
#include <list>
#include <memory>
#include <vector>
#include "SceneLayer.h"

struct BITMAP;

namespace RTE {

	/// Terrain and fog state at a completed tick. Pixel data is copied without image encoding.
	struct TerrainLayerSnapshot {
		int width = 0;
		int height = 0;
		std::vector<uint8_t> mat;
		std::vector<uint8_t> fg;
		std::vector<uint8_t> bg;
		std::vector<uint8_t> materialCopy;
		struct Layer {
			int width = 0, height = 0;
			std::vector<uint8_t> pixels;
			bool masked = true, wrapX = false, wrapY = false;
			Vector origin, offset, scrollInfo, scrollRatio, scale, scaledDimensions;
			float zOrder = 0;
			std::string entityState, contentState;
			std::string SaveCheckpoint() const;
			bool LoadCheckpoint(std::string_view text, bool validateOnly = false);
		};
		std::array<Layer, 4> unseen;
		std::array<Layer, 3> terrainLayers;
		std::deque<Box> updatedMaterialAreas;
		int orbitDirection = 0;
		int layerToDraw = 0;
		std::array<std::list<Vector>, 4> seenPixels;
		std::array<std::list<Vector>, 4> cleanedPixels;
		std::array<Vector, 4> unseenPixelSize;
		std::array<bool, 4> scanScheduled{};

		static void CopyFrom(BITMAP* bitmap, std::vector<uint8_t>& out);
		static bool CopyTo(BITMAP* bitmap, const std::vector<uint8_t>& in);

		bool Capture();
		bool CanRestore() const;
		bool Restore() const;
		std::string SaveMetadata() const;
		bool LoadMetadata(std::string_view text, bool validateOnly = false);
	private:
		static void CaptureLayer(SceneLayer* source, Layer& target, bool pixels);
		static void RestoreLayer(SceneLayer* target, const Layer& source);
	};
} // namespace RTE
