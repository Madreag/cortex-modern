#include "TerrainDebris.h"
#include "SLTerrain.h"
#include "ScenarioRunner.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace RTE;

namespace {

	struct DebrisPlacementDump {
		int rngPosX = 0;
		int rngDepth = 0;
		int buriedDepthOffset = 0;
		float centerX = 0.0F;
		float centerY = 0.0F;
		int seq = 0;
	};

	DebrisPlacementDump g_DebrisPlacementDump;

	std::string JsonEscape(const std::string& text) {
		std::string out;
		out.reserve(text.size() + 8);
		for (char ch: text) {
			if (ch == '\\' || ch == '"') {
				out.push_back('\\');
			}
			if (ch == '\n') {
				out += "\\n";
				continue;
			}
			out.push_back(ch);
		}
		return out;
	}

	void WriteDebrisStampDump(const std::string& preset, const std::string& modulePreset, const std::string& debrisFile, int material, int targetMaterial, int frame, int frameW, int frameH, const Vector& position, int offsetX, int offsetY, int dimensions, BITMAP* transformed, bool didHFlip, bool didVFlip, bool didRotate, bool drewHFlip, bool drewVFlip, float hFlipDraw, float vFlipDraw, int angleDeg, float allegroAngle, std::int32_t fixedAngle, int minRotation, int maxRotation, bool canHFlip, bool canVFlip, float flipChance) {
		if (!std::getenv("CC_TERRAIN_DUMP")) {
			return;
		}
		const int drawX = position.GetFloorIntX() - offsetX;
		const int drawY = position.GetFloorIntY() - offsetY;
		const std::string base = !ScenarioRunner::GetArgs().outPath.empty() ? ScenarioRunner::GetArgs().outPath : std::string("sim");
		std::ofstream out(base + ".load.stamps.jsonl", std::ios::binary | std::ios::app);
		char hFlipDrawBuf[32];
		char vFlipDrawBuf[32];
		char allegroBuf[32];
		char flipChanceBuf[32];
		if (drewHFlip) {
			std::snprintf(hFlipDrawBuf, sizeof(hFlipDrawBuf), "%.9g", static_cast<double>(hFlipDraw));
		} else {
			std::snprintf(hFlipDrawBuf, sizeof(hFlipDrawBuf), "null");
		}
		if (drewVFlip) {
			std::snprintf(vFlipDrawBuf, sizeof(vFlipDrawBuf), "%.9g", static_cast<double>(vFlipDraw));
		} else {
			std::snprintf(vFlipDrawBuf, sizeof(vFlipDrawBuf), "null");
		}
		if (didRotate) {
			std::snprintf(allegroBuf, sizeof(allegroBuf), "%.9g", static_cast<double>(allegroAngle));
		} else {
			std::snprintf(allegroBuf, sizeof(allegroBuf), "null");
		}
		std::snprintf(flipChanceBuf, sizeof(flipChanceBuf), "%.9g", static_cast<double>(flipChance));
		++g_DebrisPlacementDump.seq;
		out << "{\"seq\":" << g_DebrisPlacementDump.seq
		    << ",\"preset\":\"" << JsonEscape(preset) << "\""
		    << ",\"module_preset\":\"" << JsonEscape(modulePreset) << "\""
		    << ",\"debris_file\":\"" << JsonEscape(debrisFile) << "\""
		    << ",\"material\":" << material
		    << ",\"target_material\":" << targetMaterial
		    << ",\"frame\":" << frame
		    << ",\"frame_w\":" << frameW
		    << ",\"frame_h\":" << frameH
		    << ",\"rng_pos_x\":" << g_DebrisPlacementDump.rngPosX
		    << ",\"rng_depth\":" << g_DebrisPlacementDump.rngDepth
		    << ",\"buried_depth_offset\":" << g_DebrisPlacementDump.buriedDepthOffset
		    << ",\"center_x\":" << g_DebrisPlacementDump.centerX
		    << ",\"center_y\":" << g_DebrisPlacementDump.centerY
		    << ",\"pos_x\":" << position.GetFloorIntX()
		    << ",\"pos_y\":" << position.GetFloorIntY()
		    << ",\"draw_x\":" << drawX
		    << ",\"draw_y\":" << drawY
		    << ",\"dim\":" << dimensions
		    << ",\"hflip\":" << (didHFlip ? "true" : "false")
		    << ",\"vflip\":" << (didVFlip ? "true" : "false")
		    << ",\"rotate\":" << (didRotate ? "true" : "false")
		    << ",\"hflip_draw\":" << hFlipDrawBuf
		    << ",\"vflip_draw\":" << vFlipDrawBuf
		    << ",\"angle_deg\":" << (didRotate ? std::to_string(angleDeg) : std::string("null"))
		    << ",\"allegro_angle\":" << allegroBuf
		    << ",\"fixed_angle\":" << (didRotate ? std::to_string(fixedAngle) : std::string("null"))
		    << ",\"min_rotation\":" << minRotation
		    << ",\"max_rotation\":" << maxRotation
		    << ",\"can_hflip\":" << (canHFlip ? "true" : "false")
		    << ",\"can_vflip\":" << (canVFlip ? "true" : "false")
		    << ",\"flip_chance\":" << flipChanceBuf
		    << ",\"hits\":[";
		static const int kMeasuredX[] = {1990, 102, 831, 151, 1215, 2408, 1052, 2487, 1154, 233, 1364, 2450};
		static const int kMeasuredY[] = {699, 809, 868, 946, 960, 981, 987, 1032, 1068, 1108, 1120, 1139};
		bool firstHit = true;
		if (transformed) {
			for (int i = 0; i < 12; ++i) {
				const int lx = kMeasuredX[i] - drawX;
				const int ly = kMeasuredY[i] - drawY;
				if (lx < 0 || ly < 0 || lx >= dimensions || ly >= dimensions) {
					continue;
				}
				const int src = _getpixel(transformed, lx, ly);
				if (src == 0) {
					continue;
				}
				if (!firstHit) {
					out << ",";
				}
				firstHit = false;
				out << "{\"x\":" << kMeasuredX[i] << ",\"y\":" << kMeasuredY[i] << ",\"src\":" << src << "}";
			}
		}
		out << "]}\n";
	}

} // namespace

ConcreteClassInfo(TerrainDebris, Entity, 0);

TerrainDebris::TerrainDebris() {
	Clear();
}

TerrainDebris::~TerrainDebris() {
	Destroy(true);
}

void TerrainDebris::Clear() {
	m_DebrisFile.Reset();
	m_Bitmaps.clear();
	m_BitmapCount = 0;
	m_Material.Reset();
	m_TargetMaterial.Reset();
	m_DebrisPlacementMode = DebrisPlacementMode::NoPlacementRestrictions;
	m_OnlyBuried = false;
	m_MinDepth = 0;
	m_MaxDepth = 10;
	m_MinRotation = 0;
	m_MaxRotation = 0;
	m_CanHFlip = false;
	m_CanVFlip = false;
	m_FlipChance = 0.5F;
	m_Density = 0.01F;
}

int TerrainDebris::Create() {
	Entity::Create();
	m_DebrisFile.GetAsAnimation(m_Bitmaps, m_BitmapCount);
	RTEAssert(!m_Bitmaps.empty() && m_Bitmaps[0], "Failed to load debris bitmaps during TerrainDebris::Create!");
	return 0;
}

int TerrainDebris::Create(const TerrainDebris& reference) {
	Entity::Create(reference);

	m_DebrisFile = reference.m_DebrisFile;
	m_Bitmaps.clear();
	m_Bitmaps = reference.m_Bitmaps;
	m_BitmapCount = reference.m_BitmapCount;
	m_Material = reference.m_Material;
	m_TargetMaterial = reference.m_TargetMaterial;
	m_DebrisPlacementMode = reference.m_DebrisPlacementMode;
	m_OnlyBuried = reference.m_OnlyBuried;
	m_MinDepth = reference.m_MinDepth;
	m_MaxDepth = reference.m_MaxDepth;
	m_MinRotation = reference.m_MinRotation;
	m_MaxRotation = reference.m_MaxRotation;
	m_CanHFlip = reference.m_CanHFlip;
	m_CanVFlip = reference.m_CanVFlip;
	m_FlipChance = reference.m_FlipChance;
	m_Density = reference.m_Density;

	return 0;
}

int TerrainDebris::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return Entity::ReadProperty(propName, reader));

	MatchProperty("DebrisFile", { reader >> m_DebrisFile; });
	MatchProperty("DebrisPieceCount", {
		reader >> m_BitmapCount;
		m_Bitmaps.reserve(m_BitmapCount);
	});
	MatchProperty("DebrisMaterial", { reader >> m_Material; });
	MatchProperty("TargetMaterial", { reader >> m_TargetMaterial; });
	MatchProperty("DebrisPlacementMode", {
		m_DebrisPlacementMode = static_cast<DebrisPlacementMode>(std::stoi(reader.ReadPropValue()));
		if (m_DebrisPlacementMode < DebrisPlacementMode::NoPlacementRestrictions || m_DebrisPlacementMode > DebrisPlacementMode::OnOverhangAndCavityOverhang) {
			reader.ReportError("Invalid TerrainDebris placement mode!");
		}
	});
	MatchProperty("OnlyBuried", { reader >> m_OnlyBuried; });
	MatchProperty("MinDepth", { reader >> m_MinDepth; });
	MatchProperty("MaxDepth", { reader >> m_MaxDepth; });
	MatchProperty("MinRotation", { reader >> m_MinRotation; });
	MatchProperty("MaxRotation", { reader >> m_MaxRotation; });
	MatchProperty("CanHFlip", { reader >> m_CanHFlip; });
	MatchProperty("CanVFlip", { reader >> m_CanVFlip; });
	MatchProperty("FlipChance", { reader >> m_FlipChance; });
	MatchProperty("DensityPerMeter", { reader >> m_Density; });

	EndPropertyList;
}

int TerrainDebris::Save(Writer& writer) const {
	Entity::Save(writer);

	writer.NewPropertyWithValue("DebrisFile", m_DebrisFile);
	writer.NewPropertyWithValue("DebrisPieceCount", m_BitmapCount);
	writer.NewPropertyWithValue("DebrisMaterial", m_Material);
	writer.NewPropertyWithValue("TargetMaterial", m_TargetMaterial);
	writer.NewPropertyWithValue("DebrisPlacementMode", m_DebrisPlacementMode);
	writer.NewPropertyWithValue("OnlyBuried", m_OnlyBuried);
	writer.NewPropertyWithValue("MinDepth", m_MinDepth);
	writer.NewPropertyWithValue("MaxDepth", m_MaxDepth);
	writer.NewPropertyWithValue("MinRotation", m_MinRotation);
	writer.NewPropertyWithValue("MaxRotation", m_MaxRotation);
	writer.NewPropertyWithValue("CanHFlip", m_CanHFlip);
	writer.NewPropertyWithValue("CanVFlip", m_CanVFlip);
	writer.NewPropertyWithValue("FlipChance", m_FlipChance);
	writer.NewPropertyWithValue("DensityPerMeter", m_Density);

	return 0;
}

bool TerrainDebris::GetPiecePlacementPosition(SLTerrain* terrain, Box& possiblePiecePosition) const {
	BITMAP* matBitmap = terrain->GetMaterialBitmap();
	// Debris placement writes terrain pixels — sim state, so draw the sim RNG explicitly.
	int posX = g_SimRNG.RandomNum<int>(0, matBitmap->w);
	int depth = g_SimRNG.RandomNum<int>(m_MinDepth, m_MaxDepth);
	int buriedDepthOffset = m_OnlyBuried ? static_cast<int>(possiblePiecePosition.GetHeight() * 0.6F) : 0;
	int prevMaterialCheckPixel = -1;

	bool scanForOverhang = m_DebrisPlacementMode == DebrisPlacementMode::OnOverhangOnly || m_DebrisPlacementMode == DebrisPlacementMode::OnCavityOverhangOnly || m_DebrisPlacementMode == DebrisPlacementMode::OnOverhangAndCavityOverhang;

	// For overhangs scan from the bottom so it's easier to detect.
	for (int surfacePosY = 0, overhangPosY = matBitmap->h - 1; surfacePosY < matBitmap->h && overhangPosY > 0; surfacePosY++, overhangPosY--) {
		int posY = scanForOverhang ? overhangPosY : surfacePosY;
		int materialCheckPixel = _getpixel(matBitmap, posX, posY);
		if (materialCheckPixel != MaterialColorKeys::g_MaterialAir) {
			// TODO: Adding depth here will properly place debris only on target material, rather than any random pixel within depth range from the original target material pixel.
			int depthAdjustedPosY = posY; // + (depth * (scanForOverhang ? -1 : 1));
			if (scanForOverhang ? (depthAdjustedPosY > 0) : (depthAdjustedPosY < matBitmap->h)) {
				// TODO: Enable the depth check once multi TargetMaterial debris is supported.
				// materialCheckPixel = _getpixel(matBitmap, posX, depthAdjustedPosY);
				if (MaterialPixelIsValidTarget(materialCheckPixel, prevMaterialCheckPixel)) {
					surfacePosY += (depth + buriedDepthOffset);
					overhangPosY -= (depth + buriedDepthOffset);
					possiblePiecePosition.SetCenter(Vector(static_cast<float>(posX), static_cast<float>(scanForOverhang ? overhangPosY : surfacePosY)));
					if (!m_OnlyBuried || terrain->IsBoxBuried(possiblePiecePosition)) {
						if (std::getenv("CC_TERRAIN_DUMP")) {
							g_DebrisPlacementDump.rngPosX = posX;
							g_DebrisPlacementDump.rngDepth = depth;
							g_DebrisPlacementDump.buriedDepthOffset = buriedDepthOffset;
							g_DebrisPlacementDump.centerX = static_cast<float>(posX);
							g_DebrisPlacementDump.centerY = static_cast<float>(scanForOverhang ? overhangPosY : surfacePosY);
						}
						return true;
					}
				}
			}
		}
		prevMaterialCheckPixel = materialCheckPixel;
	}
	return false;
}

bool TerrainDebris::MaterialPixelIsValidTarget(int materialCheckPixel, int prevMaterialCheckPixel) const {
	bool checkResult = true;

	if (materialCheckPixel != m_TargetMaterial.GetIndex()) {
		checkResult = false;
	} else {
		// TODO: Consider using disgustang bitwise/shifting junk instead of enum.
		switch (m_DebrisPlacementMode) {
			case DebrisPlacementMode::OnSurfaceOnly:
			case DebrisPlacementMode::OnOverhangOnly:
				if (prevMaterialCheckPixel != MaterialColorKeys::g_MaterialAir) {
					checkResult = false;
				}
				break;
			case DebrisPlacementMode::OnCavitySurfaceOnly:
			case DebrisPlacementMode::OnCavityOverhangOnly:
				if (prevMaterialCheckPixel != MaterialColorKeys::g_MaterialCavity) {
					checkResult = false;
				}
				break;
			case DebrisPlacementMode::OnSurfaceAndCavitySurface:
			case DebrisPlacementMode::OnOverhangAndCavityOverhang:
				if (prevMaterialCheckPixel != MaterialColorKeys::g_MaterialAir && prevMaterialCheckPixel != MaterialColorKeys::g_MaterialCavity) {
					checkResult = false;
				}
				break;
			default:
				// No placement restrictions, pixel just needs to be of target material.
				break;
		}
	}
	return checkResult;
}

void TerrainDebris::DrawToTerrain(SLTerrain* terrain, BITMAP* bitmapToDraw, const Vector& position) const {
	// Create a square temp bitmap that is larger than the original to avoid clipping if rotating.
	int dimensions = 10 + std::max(bitmapToDraw->w, bitmapToDraw->h);
	BITMAP* tempDrawBitmap = create_bitmap_ex(8, dimensions, dimensions);
	clear_bitmap(tempDrawBitmap);

	// Offset the original bitmap on the temp bitmap so it's centered, otherwise will be positioned incorrectly and can clip if rotated or flipped.
	int offsetX = (dimensions - bitmapToDraw->w) / 2;
	int offsetY = (dimensions - bitmapToDraw->h) / 2;
	blit(bitmapToDraw, tempDrawBitmap, 0, 0, offsetX, offsetY, bitmapToDraw->w, bitmapToDraw->h);

	BITMAP* tempFlipAndRotBitmap = nullptr;
	bool didHFlip = false;
	bool didVFlip = false;
	bool didRotate = false;
	bool drewHFlip = false;
	bool drewVFlip = false;
	float hFlipDraw = 0.0F;
	float vFlipDraw = 0.0F;
	int angleDeg = 0;
	float allegroAngle = 0.0F;
	std::int32_t fixedAngle = 0;
	if (m_CanHFlip || m_CanVFlip || m_MinRotation != 0 || m_MaxRotation != 0) {
		tempFlipAndRotBitmap = create_bitmap_ex(8, dimensions, dimensions);

		if (m_CanHFlip) {
			hFlipDraw = RandomNum();
			drewHFlip = true;
			if (m_FlipChance >= hFlipDraw) {
				didHFlip = true;
				clear_bitmap(tempFlipAndRotBitmap);
				draw_sprite_h_flip(tempFlipAndRotBitmap, tempDrawBitmap, 0, 0);
				blit(tempFlipAndRotBitmap, tempDrawBitmap, 0, 0, 0, 0, dimensions, dimensions);
			}
		}
		if (m_CanVFlip) {
			vFlipDraw = RandomNum();
			drewVFlip = true;
			if (m_FlipChance >= vFlipDraw) {
				didVFlip = true;
				clear_bitmap(tempFlipAndRotBitmap);
				draw_sprite_v_flip(tempFlipAndRotBitmap, tempDrawBitmap, 0, 0);
				blit(tempFlipAndRotBitmap, tempDrawBitmap, 0, 0, 0, 0, dimensions, dimensions);
			}
		}
		if (m_MinRotation != 0 || m_MaxRotation != 0) {
			clear_bitmap(tempFlipAndRotBitmap);
			angleDeg = RandomNum(m_MinRotation, m_MaxRotation);
			allegroAngle = GetAllegroAngle(static_cast<float>(angleDeg));
			const fixed angleFixed = ftofix(allegroAngle);
			fixedAngle = static_cast<std::int32_t>(angleFixed);
			didRotate = true;
			rotate_sprite(tempFlipAndRotBitmap, tempDrawBitmap, 0, 0, angleFixed);
			blit(tempFlipAndRotBitmap, tempDrawBitmap, 0, 0, 0, 0, dimensions, dimensions);
		}
	}
	draw_sprite(terrain->GetFGColorBitmap(), tempDrawBitmap, position.GetFloorIntX() - offsetX, position.GetFloorIntY() - offsetY);
	draw_character_ex(terrain->GetMaterialBitmap(), tempDrawBitmap, position.GetFloorIntX() - offsetX, position.GetFloorIntY() - offsetY, m_Material.GetIndex(), -1);

	if (std::getenv("CC_TERRAIN_DUMP")) {
		int frame = -1;
		for (int i = 0; i < m_BitmapCount; ++i) {
			if (m_Bitmaps[i] == bitmapToDraw) {
				frame = i;
				break;
			}
		}
		WriteDebrisStampDump(GetPresetName(), GetModuleAndPresetName(), m_DebrisFile.GetDataPath(), static_cast<int>(m_Material.GetIndex()), static_cast<int>(m_TargetMaterial.GetIndex()), frame, bitmapToDraw->w, bitmapToDraw->h, position, offsetX, offsetY, dimensions, tempDrawBitmap, didHFlip, didVFlip, didRotate, drewHFlip, drewVFlip, hFlipDraw, vFlipDraw, angleDeg, allegroAngle, fixedAngle, m_MinRotation, m_MaxRotation, m_CanHFlip, m_CanVFlip, m_FlipChance);
	}

	if (tempFlipAndRotBitmap) {
		destroy_bitmap(tempFlipAndRotBitmap);
	}
	destroy_bitmap(tempDrawBitmap);
}

void TerrainDebris::ScatterOnTerrain(SLTerrain* terrain) {
	RTEAssert(!m_Bitmaps.empty() && m_BitmapCount > 0, "No bitmaps loaded for terrain debris during TerrainDebris::ScatterOnTerrain!");

	int possiblePieceToPlaceCount = static_cast<int>((static_cast<float>(terrain->GetMaterialBitmap()->w) * c_MPP) * m_Density);
	for (int piece = 0; piece < possiblePieceToPlaceCount; ++piece) {
		int pieceBitmapIndex = RandomNum(0, m_BitmapCount - 1);
		RTEAssert(pieceBitmapIndex >= 0 && pieceBitmapIndex < m_BitmapCount, "Bitmap index was out of bounds during TerrainDebris::ScatterOnTerrain!");
		Box possiblePiecePosition(Vector(), static_cast<float>(m_Bitmaps[pieceBitmapIndex]->w), static_cast<float>(m_Bitmaps.at(pieceBitmapIndex)->h));
		if (GetPiecePlacementPosition(terrain, possiblePiecePosition)) {
			DrawToTerrain(terrain, m_Bitmaps[pieceBitmapIndex], possiblePiecePosition.GetCorner());
		}
	}
}
