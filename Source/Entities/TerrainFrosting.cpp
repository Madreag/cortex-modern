#include "TerrainFrosting.h"
#include "CheckpointNativeSnapshot.h"
#include "Material.h"
#include "CheckpointImage.h"
#include "SLTerrain.h"

using namespace RTE;

const std::string TerrainFrosting::c_ClassName = "TerrainFrosting";

void TerrainFrosting::TouchCheckpoint() { CheckpointValueWritten(this); }

TerrainFrosting::TerrainFrosting(const TerrainFrosting& source, CheckpointNativeSnapshot& snapshot) :
	m_FrostingMaterial(snapshot.Freeze(source.m_FrostingMaterial)),
	m_TargetMaterial(snapshot.Freeze(source.m_TargetMaterial)),
	m_MinThickness(snapshot.Freeze(source.m_MinThickness)),
	m_MaxThickness(snapshot.Freeze(source.m_MaxThickness)),
	m_InAirOnly(snapshot.Freeze(source.m_InAirOnly)),
	m_CheckpointInitialized(snapshot.Freeze(source.m_CheckpointInitialized)) {
}

void TerrainFrosting::PrepareCheckpointNative(const TerrainFrosting& source, TerrainFrosting* target, CheckpointNativeSnapshot& snapshot) {
	snapshot.Prepare(source.m_FrostingMaterial, &target->m_FrostingMaterial);
	snapshot.Prepare(source.m_TargetMaterial, &target->m_TargetMaterial);
}

void TerrainFrosting::Clear() {
	CheckpointChange changed(*this, [this] { return CheckpointFields(m_FrostingMaterial, m_TargetMaterial, m_MinThickness, m_MaxThickness, m_InAirOnly); }, m_CheckpointInitialized);
	m_CheckpointInitialized = true;
	m_FrostingMaterial.Reset();
	m_TargetMaterial.Reset();
	m_MinThickness = 5;
	m_MaxThickness = 5;
	m_InAirOnly = true;
}

int TerrainFrosting::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return Serializable::ReadProperty(propName, reader));

	MatchProperty("FrostingMaterial", { reader >> m_FrostingMaterial; });
	MatchProperty("TargetMaterial", { reader >> m_TargetMaterial; });
	MatchProperty("MinThickness", { reader >> m_MinThickness; });
	MatchProperty("MaxThickness", { reader >> m_MaxThickness; });
	MatchProperty("InAirOnly", { reader >> m_InAirOnly; });

	EndPropertyList;
}

int TerrainFrosting::Save(Writer& writer) const {
	Serializable::Save(writer);

	writer.NewPropertyWithValue("FrostingMaterial", m_FrostingMaterial);
	writer.NewPropertyWithValue("TargetMaterial", m_TargetMaterial);
	writer.NewPropertyWithValue("MinThickness", m_MinThickness);
	writer.NewPropertyWithValue("MaxThickness", m_MaxThickness);
	writer.NewPropertyWithValue("InAirOnly", m_InAirOnly);

	return 0;
}

void TerrainFrosting::FrostTerrain(SLTerrain* terrain) const {
	BITMAP* frostingTexture = m_FrostingMaterial.GetFGTexture();
	BITMAP* fgColorBitmap = terrain->GetFGColorBitmap();
	BITMAP* matBitmap = terrain->GetBitmap();

	bool targetMatFound = false;
	bool applyingFrosting = false;
	int appliedThickness = 0;

	for (int xPos = 0; xPos < matBitmap->w; ++xPos) {
		// Frosting thickness writes terrain pixels — sim state, so draw the sim RNG explicitly.
		int thicknessGoal = g_SimRNG.RandomNum<int>(m_MinThickness, m_MaxThickness);

		for (int yPos = matBitmap->h - 1; yPos >= 0; --yPos) {
			int materialCheckPixel = _getpixel(matBitmap, xPos, yPos);

			if (!targetMatFound && materialCheckPixel == m_TargetMaterial.GetIndex()) {
				targetMatFound = true;
				appliedThickness = 0;
			} else if (targetMatFound && materialCheckPixel != m_TargetMaterial.GetIndex() && appliedThickness <= thicknessGoal) {
				targetMatFound = false;
				applyingFrosting = true;
			}
			if (applyingFrosting && (materialCheckPixel == MaterialColorKeys::g_MaterialAir || !m_InAirOnly) && appliedThickness <= thicknessGoal) {
				_putpixel(fgColorBitmap, xPos, yPos, frostingTexture ? _getpixel(frostingTexture, xPos % frostingTexture->w, yPos % frostingTexture->h) : m_FrostingMaterial.GetColor().GetIndex());
				_putpixel(matBitmap, xPos, yPos, m_FrostingMaterial.GetIndex());
				appliedThickness++;
			} else {
				applyingFrosting = false;
			}
		}
	}
}
