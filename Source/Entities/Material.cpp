#include "Material.h"
#include "CheckpointArchive.h"
#include "MovableObject.h"
#include "Base64/base64.h"
#include "Constants.h"

using namespace RTE;

ConcreteClassInfo(Material, Entity, 0);

void Material::Clear() {
	m_Index = 0;
	m_Priority = -1;
	m_Piling = 0;
	m_Integrity = 0.0F;
	m_Restitution = 0.0F;
	m_Friction = 0.0F;
	m_Stickiness = 0.0F;
	m_VolumeDensity = 0.0F;
	m_PixelDensity = 0.0F;
	m_GibImpulseLimitPerLiter = 0.0F;
	m_GibWoundLimitPerLiter = 0.0F;
	m_SettleMaterialIndex = 0;
	m_SpawnMaterialIndex = 0;
	m_IsScrap = false;
	m_Color.Reset();
	m_UseOwnColor = false;
	m_FGTextureFile.Reset();
	m_BGTextureFile.Reset();
	m_TerrainFGTexture = nullptr;
	m_TerrainBGTexture = nullptr;
}

int Material::Create(const Material& reference) {
	Entity::Create(reference);
	if (MovableObject::IsFaithfulClone()) Entity::LoadCheckpoint(reference.Entity::SaveCheckpoint());

	m_Index = reference.m_Index;
	m_Priority = reference.m_Priority;
	m_Piling = reference.m_Piling;
	m_Integrity = reference.m_Integrity;
	m_Restitution = reference.m_Restitution;
	m_Friction = reference.m_Friction;
	m_Stickiness = reference.m_Stickiness;
	m_VolumeDensity = reference.m_VolumeDensity;
	m_PixelDensity = reference.m_PixelDensity;
	m_GibImpulseLimitPerLiter = reference.m_GibImpulseLimitPerLiter;
	m_GibWoundLimitPerLiter = reference.m_GibWoundLimitPerLiter;
	m_SettleMaterialIndex = reference.m_SettleMaterialIndex;
	m_SpawnMaterialIndex = reference.m_SpawnMaterialIndex;
	m_IsScrap = reference.m_IsScrap;
	m_Color = reference.m_Color;
	m_UseOwnColor = reference.m_UseOwnColor;
	m_FGTextureFile = reference.m_FGTextureFile;
	m_BGTextureFile = reference.m_BGTextureFile;
	m_TerrainFGTexture = reference.m_TerrainFGTexture;
	m_TerrainBGTexture = reference.m_TerrainBGTexture;

	return 0;
}

int Material::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return Entity::ReadProperty(propName, reader));
    MatchProperty("SpecialBehaviour_MaterialCheckpoint", {
        if (!LoadCheckpoint(base64_decode(reader.ReadPropValue()))) reader.ReportError("invalid Material checkpoint");
    });

	MatchProperty("Index", {
		// TODO: Check for index collisions here
		reader >> m_Index;
	});
	MatchProperty("Priority", { reader >> m_Priority; });
	MatchProperty("Piling", { reader >> m_Piling; });
	MatchForwards("Integrity") MatchProperty("StructuralIntegrity", {
		reader >> m_Integrity;
		m_Integrity = (m_Integrity == -1.0F) ? std::numeric_limits<float>::max() : m_Integrity;
	});
	MatchForwards("Restitution") MatchProperty("Bounce", { reader >> m_Restitution; });
	MatchProperty("Friction", { reader >> m_Friction; });
	MatchProperty("Stickiness", { reader >> m_Stickiness; });
	MatchProperty("DensityKGPerVolumeL", {
		reader >> m_VolumeDensity;
		// Overrides the pixel density
		m_PixelDensity = m_VolumeDensity * c_LPP;
	});
	MatchProperty("DensityKGPerPixel", {
		reader >> m_PixelDensity;
		// Overrides the volume density
		m_VolumeDensity = m_PixelDensity * c_PPL;
	});
	MatchProperty("GibImpulseLimitPerVolumeL", { reader >> m_GibImpulseLimitPerLiter; });
	MatchProperty("GibWoundLimitPerVolumeL", { reader >> m_GibWoundLimitPerLiter; });
	MatchProperty("SettleMaterial", { reader >> m_SettleMaterialIndex; });
	MatchForwards("SpawnMaterial") MatchProperty("TransformsInto", { reader >> m_SpawnMaterialIndex; });
	MatchProperty("IsScrap", { reader >> m_IsScrap; });
	MatchProperty("Color", { reader >> m_Color; });
	MatchProperty("UseOwnColor", { reader >> m_UseOwnColor; });
	MatchProperty("FGTextureFile", {
		reader >> m_FGTextureFile;
		m_TerrainFGTexture = m_FGTextureFile.GetAsBitmap();
	});
	MatchProperty("BGTextureFile", {
		reader >> m_BGTextureFile;
		m_TerrainBGTexture = m_BGTextureFile.GetAsBitmap();
	});

	EndPropertyList;
}

int Material::Save(Writer& writer) const {
	Entity::Save(writer);
	// Materials should never be altered, so no point in saving additional properties when it's a copy
	if (m_IsOriginalPreset) {
		writer.NewPropertyWithValue("Priority", m_Priority);
		writer.NewPropertyWithValue("Piling", m_Piling);
		writer.NewPropertyWithValue("StructuralIntegrity", m_Integrity);
		writer.NewPropertyWithValue("Restitution", m_Restitution);
		writer.NewPropertyWithValue("Friction", m_Friction);
		writer.NewPropertyWithValue("Stickiness", m_Stickiness);
		writer.NewPropertyWithValue("DensityKGPerVolumeL", m_VolumeDensity);
		writer.NewPropertyWithValue("GibImpulseLimitPerVolumeL", m_GibImpulseLimitPerLiter);
		writer.NewPropertyWithValue("GibWoundLimitPerVolumeL", m_GibWoundLimitPerLiter);
		writer.NewPropertyWithValue("SettleMaterial", m_SettleMaterialIndex);
		writer.NewPropertyWithValue("SpawnMaterial", m_SpawnMaterialIndex);
		writer.NewPropertyWithValue("IsScrap", m_IsScrap);
		writer.NewPropertyWithValue("Color", m_Color);
		writer.NewPropertyWithValue("UseOwnColor", m_UseOwnColor);
		writer.NewPropertyWithValue("FGTextureFile", m_FGTextureFile);
		writer.NewPropertyWithValue("BGTextureFile", m_BGTextureFile);
	}
	if (writer.IsSnapshot()) writer.NewPropertyWithValue("SpecialBehaviour_MaterialCheckpoint", base64_encode(SaveCheckpoint(), true));
	return 0;
}

void Material::SwapCheckpoint(Material& other) noexcept {
    using std::swap;
    swap(m_PresetName, other.m_PresetName); swap(m_CopiedFromPresetName, other.m_CopiedFromPresetName);
    swap(m_PresetDescription, other.m_PresetDescription); swap(m_FormattedReaderPosition, other.m_FormattedReaderPosition);
    swap(m_IsOriginalPreset, other.m_IsOriginalPreset); swap(m_DefinedInModule, other.m_DefinedInModule);
    swap(m_Groups, other.m_Groups); swap(m_RandomWeight, other.m_RandomWeight);
    swap(m_Index, other.m_Index); swap(m_Priority, other.m_Priority); swap(m_Piling, other.m_Piling);
    swap(m_Integrity, other.m_Integrity); swap(m_Restitution, other.m_Restitution);
    swap(m_Friction, other.m_Friction); swap(m_Stickiness, other.m_Stickiness);
    swap(m_VolumeDensity, other.m_VolumeDensity); swap(m_PixelDensity, other.m_PixelDensity);
    swap(m_GibImpulseLimitPerLiter, other.m_GibImpulseLimitPerLiter); swap(m_GibWoundLimitPerLiter, other.m_GibWoundLimitPerLiter);
    swap(m_SettleMaterialIndex, other.m_SettleMaterialIndex); swap(m_SpawnMaterialIndex, other.m_SpawnMaterialIndex);
    swap(m_IsScrap, other.m_IsScrap); swap(m_UseOwnColor, other.m_UseOwnColor);
    m_Color.SwapCheckpoint(other.m_Color);
    const auto swapFile = [](ContentFile& left, ContentFile& right) {
        using std::swap;
        swap(left.m_DataPath, right.m_DataPath); swap(left.m_DataPathExtension, right.m_DataPathExtension);
        swap(left.m_DataPathWithoutExtension, right.m_DataPathWithoutExtension); swap(left.m_DataPathIsImageFile, right.m_DataPathIsImageFile);
        swap(left.m_ImageFileInfo, right.m_ImageFileInfo); swap(left.m_FormattedReaderPosition, right.m_FormattedReaderPosition);
        swap(left.m_DataPathAndReaderPosition, right.m_DataPathAndReaderPosition); swap(left.m_DataModuleID, right.m_DataModuleID);
        swap(left.m_IsMemoryPNG, right.m_IsMemoryPNG);
    };
    swapFile(m_FGTextureFile, other.m_FGTextureFile); swapFile(m_BGTextureFile, other.m_BGTextureFile);
    swap(m_TerrainFGTexture, other.m_TerrainFGTexture); swap(m_TerrainBGTexture, other.m_TerrainBGTexture);
}

std::string Material::SaveCheckpoint() const {
    CheckpointWriter archive("Material1");
    archive(Entity::SaveCheckpoint());
    VisitCheckpoint(archive, *this);
    const auto textureKey = [](const BITMAP* bitmap) {
        if (!bitmap) return std::string{};
        std::string result;
        for (const auto& [path, cached]: ContentFile::s_LoadedBitmaps[ContentFile::BitDepths::Eight]) if (cached == bitmap && (result.empty() || path < result)) result = path;
        if (result.empty()) throw std::runtime_error("material texture is not owned by the content cache");
        return result;
    };
    archive(textureKey(m_TerrainFGTexture), textureKey(m_TerrainBGTexture));
    return archive.Text();
}

bool Material::LoadCheckpoint(std::string_view text, bool validateOnly) {
    try {
        CheckpointReader archive(text, "Material1", validateOnly);
        std::string identity;
        archive.Value(identity);
        if (!Entity::LoadCheckpoint(identity, true)) return false;
        archive.OnCommit([this, identity] { Entity::LoadCheckpoint(identity); });
        VisitCheckpoint(archive, *this);
        std::string foreground, background;
        archive.Value(foreground); archive.Value(background);
        // Texture pointers are borrowed cache aliases, independent of ContentFile metadata.
        // Resolve every dependency before applying any field to the destination Material.
        const auto texture = [](const std::string& path) -> BITMAP* {
            if (path.empty()) return nullptr;
            const auto& cache = ContentFile::s_LoadedBitmaps[ContentFile::BitDepths::Eight];
            const auto found = cache.find(path);
            if (found == cache.end() || !found->second) throw std::runtime_error("missing cached material texture");
            return found->second;
        };
        BITMAP* foregroundBitmap = texture(foreground);
        BITMAP* backgroundBitmap = texture(background);
        archive.OnCommit([this, foregroundBitmap, backgroundBitmap] {
            m_TerrainFGTexture = foregroundBitmap;
            m_TerrainBGTexture = backgroundBitmap;
        });
        archive.Finish();
        return true;
    } catch (const std::exception&) { return false; }
}
