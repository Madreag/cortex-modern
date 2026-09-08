#include "CheckpointArchive.h"
#include "SceneMan.h"
#include "PostProcessMan.h"
#include "PresetMan.h"
#include "FrameMan.h"
#include "ActivityMan.h"
#include "CameraMan.h"
#include "ConsoleMan.h"
#include "PrimitiveMan.h"
#include "SettingsMan.h"
#include "Scene.h"
#include "SLTerrain.h"
#include "SLBackground.h"
#include "TerrainObject.h"
#include "MovableObject.h"
#include "ContentFile.h"
#include "MOPixel.h"
#include "Atom.h"
#include "Material.h"
#include "SoundContainer.h"
#include "SimChecksum.h"
#include "TimerMan.h"
#include "LuaMan.h"

#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <atomic>
#include <mutex>

using namespace RTE;

#define CLEANAIRINTERVAL 200000

const std::string SceneMan::c_ClassName = "SceneMan";
std::vector<std::pair<int, BITMAP*>> SceneMan::m_IntermediateSettlingBitmaps;

// Stored as a thread-local instead of in the class, because multithreaded Lua scripts will interfere otherwise
thread_local Vector s_LastRayHitPos;

// While set on a worker, GetTerrMatter reads the frozen material copy so the threaded vision pass can't race concurrent carves.
thread_local bool s_ReadTerrainFromCopy = false;

namespace {
	struct TerrainEvent {
		uint32_t Tick;
		char Tag[6];
		int32_t X;
		int32_t Y;
		int32_t A;
		int32_t B;
		int32_t C;
	};
	// Spawn hooks can fire from threaded Lua adds, so appends take the lock.
	std::mutex s_TerrainEventsMutex;
	std::vector<TerrainEvent> s_TerrainEvents;
	uint64_t s_TerrainEventsFrom = 1;
	uint64_t s_TerrainEventsTo = 0;
	bool s_TerrainEventsFlushedAtEnd = false;
	long s_TerrainEventContextUID = 0;
} // namespace

void SceneMan::SetTerrainEventContext(long uid) {
	s_TerrainEventContextUID = uid;
}

long SceneMan::GetTerrainEventContext() {
	return s_TerrainEventContextUID;
}

const std::vector<long>& SceneMan::GetTrackedUIDs() {
	static std::vector<long> s_trackedUIDs;
	static bool s_parsed = false;
	if (!s_parsed) {
		s_parsed = true;
		if (const char* env = std::getenv("CC_TRACK_UID")) {
			const std::string list(env);
			size_t start = 0;
			while (start < list.size()) {
				size_t end = list.find(',', start);
				if (end == std::string::npos) {
					end = list.size();
				}
				const long parsed = std::strtol(list.substr(start, end - start).c_str(), nullptr, 10);
				if (parsed > 0) {
					s_trackedUIDs.push_back(parsed);
				}
				start = end + 1;
			}
		}
	}
	return s_trackedUIDs;
}

bool SceneMan::IsTrackedUID(long uid) {
	const std::vector<long>& trackedUIDs = GetTrackedUIDs();
	if (trackedUIDs.empty()) {
		return false;
	}
	return std::find(trackedUIDs.begin(), trackedUIDs.end(), uid) != trackedUIDs.end();
}

void SceneMan::TraceTerrainEvent(const char* tag, int x, int y, int a, int b, int c) {
	// First use can come from concurrent worker spawns; initialize exactly once.
	static std::once_flag s_initFlag;
	static std::atomic<int> s_state{0};
	std::call_once(s_initFlag, [] {
		const char* env = std::getenv("CC_TERRAIN_EVENTS");
		unsigned long long from = 0;
		unsigned long long to = 0;
		if (env && std::sscanf(env, "%llu:%llu", &from, &to) == 2 && to >= from) {
			s_TerrainEventsFrom = from;
			s_TerrainEventsTo = to;
			s_TerrainEvents.reserve(1 << 20);
			s_state = 1;
		} else {
			s_state = -1;
		}
	});
	if (s_state != 1) {
		return;
	}
	const uint64_t tick = static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
	if (tick < s_TerrainEventsFrom || tick > s_TerrainEventsTo) {
		return;
	}
	TerrainEvent event{};
	event.Tick = static_cast<uint32_t>(tick);
	std::snprintf(event.Tag, sizeof(event.Tag), "%s", tag);
	event.X = x;
	event.Y = y;
	event.A = a;
	event.B = b;
	event.C = c;
	std::lock_guard<std::mutex> lock(s_TerrainEventsMutex);
	s_TerrainEvents.push_back(event);
}

void SceneMan::FlushTerrainEvents(const std::string& filePath) {
	if (s_TerrainEvents.empty()) {
		return;
	}
	std::ofstream out(filePath, std::ios::trunc);
	for (const TerrainEvent& event: s_TerrainEvents) {
		out << event.Tick << " " << event.Tag << " " << event.X << "," << event.Y << " " << event.A << " " << event.B << " " << event.C << "\n";
	}
	std::cout << "[terrain-events] " << s_TerrainEvents.size() << " events -> " << filePath << std::endl;
}

void SceneMan::FlushTerrainEventsAtWindowEnd(uint64_t simTick, const std::string& filePath) {
	if (s_TerrainEventsFlushedAtEnd || s_TerrainEventsTo == 0 || simTick <= s_TerrainEventsTo) {
		return;
	}
	s_TerrainEventsFlushedAtEnd = true;
	FlushTerrainEvents(filePath);
}

SceneMan::ScopedTerrainCopyRead::ScopedTerrainCopyRead() :
    m_Previous(s_ReadTerrainFromCopy) {
	s_ReadTerrainFromCopy = true;
}

SceneMan::ScopedTerrainCopyRead::~ScopedTerrainCopyRead() {
	s_ReadTerrainFromCopy = m_Previous;
}

SceneMan::SceneMan() {
	m_pOrphanSearchBitmap = 0;
	Clear();
}

SceneMan::~SceneMan() {
	Destroy();
}

void SceneMan::Clear() {
	m_DefaultSceneName = "Tutorial Bunker";
	m_pSceneToLoad = nullptr;
	m_PlaceObjects = true;
	m_PlaceUnits = true;
	m_pCurrentScene = nullptr;
	m_pMOColorLayer = nullptr;
	m_pDebugLayer = nullptr;

	m_LayerDrawMode = g_LayerNormal;

	m_MatNameMap.clear();
	m_apMatPalette.fill(nullptr);
	m_MaterialCount = 0;

	m_MaterialCopiesVector.clear();
	m_MaterialCopyIndices.clear();
	m_RetiredMaterialCopies.clear();
	for (auto& retired: m_RetiredPaletteMaterials) retired.clear();

	m_pUnseenRevealSound = nullptr;
	m_DrawRayCastVisualizations = false;
	m_DrawPixelCheckVisualizations = false;
	m_LastUpdatedScreen = 0;
	m_SecondStructPass = false;
	//    m_CalcTimer.Reset();
	m_CleanTimer.Reset();

	if (m_pOrphanSearchBitmap)
		destroy_bitmap(m_pOrphanSearchBitmap);
	m_pOrphanSearchBitmap = create_bitmap_ex(8, MAXORPHANRADIUS, MAXORPHANRADIUS);

	m_ScrapCompactingHeight = 25;
}

void SceneMan::Initialize() const {
	// Can't create these earlier in the static declaration because allegro_init needs to be called before create_bitmap
	m_IntermediateSettlingBitmaps = {
	    {16, create_bitmap_ex(8, 16, 16)},
	    {32, create_bitmap_ex(8, 32, 32)},
	    {48, create_bitmap_ex(8, 48, 48)},
	    {64, create_bitmap_ex(8, 64, 64)},
	    {96, create_bitmap_ex(8, 96, 96)},
	    {128, create_bitmap_ex(8, 128, 128)},
	    {192, create_bitmap_ex(8, 192, 192)},
	    {256, create_bitmap_ex(8, 256, 256)},
	    {384, create_bitmap_ex(8, 384, 384)},
	    {512, create_bitmap_ex(8, 512, 512)}};
}

int SceneMan::Create(const std::string& readerFile) {
	Reader* reader = new Reader();
	if (reader->Create(readerFile.c_str()))
		g_ConsoleMan.PrintString("ERROR: Could not find Scene definition file!");

	Serializable::Create(*reader);
	delete reader;

	return 0;
}

Material* SceneMan::AddMaterialCopy(Material* mat) {
	Material* matCopy = dynamic_cast<Material*>(mat->Clone());
	if (matCopy) {
		m_MaterialCopyIndices[matCopy] = m_MaterialCopiesVector.size();
		m_MaterialCopiesVector.push_back(matCopy);
	}

	return matCopy;
}

int SceneMan::LoadScene(Scene* pNewScene, bool placeObjects, bool placeUnits) {
	if (!pNewScene) {
		return -1;
	}

	g_MovableMan.PurgeAllMOs();
	g_LuaMan.ResetPathCallbacks(true);
	g_PostProcessMan.ClearScenePostEffects();

	if (m_pCurrentScene) {
		delete m_pCurrentScene;
		m_pCurrentScene = nullptr;
	}

	m_pCurrentScene = pNewScene;
	if (m_pCurrentScene->LoadData(placeObjects, true, placeUnits) < 0) {
		g_ConsoleMan.PrintString("ERROR: Loading scene \'" + m_pCurrentScene->GetPresetName() + "\' failed! Has it been properly defined?");
		return -1;
	}

	// Report successful load to the console
	g_ConsoleMan.PrintString("SYSTEM: Scene \"" + m_pCurrentScene->GetPresetName() + "\" was loaded");

	// Set the proper scales of the unseen obscuring SceneLayers
	for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team) {
		if (!g_ActivityMan.GetActivity()->TeamActive(team))
			continue;
		SceneLayer* pUnseenLayer = m_pCurrentScene->GetUnseenLayer(team);
		if (pUnseenLayer && pUnseenLayer->GetBitmap()) {
			// Calculate how many times smaller the unseen map is compared to the entire terrain's dimensions, and set it as the scale factor on the Unseen layer
			pUnseenLayer->SetScaleFactor(Vector((float)m_pCurrentScene->GetTerrain()->GetBitmap()->w / (float)pUnseenLayer->GetBitmap()->w, (float)m_pCurrentScene->GetTerrain()->GetBitmap()->h / (float)pUnseenLayer->GetBitmap()->h));
		}
	}

	// Get the unseen reveal sound
	if (!m_pUnseenRevealSound)
		m_pUnseenRevealSound = dynamic_cast<SoundContainer*>(g_PresetMan.GetEntityPreset("SoundContainer", "Unseen Reveal Blip")->Clone());

	//    m_pCurrentScene->GetTerrain()->CleanAir();

	// Re-create the MoveableObject's color SceneLayer
	delete m_pMOColorLayer;
	BITMAP* pBitmap = create_bitmap_ex(8, GetSceneWidth(), GetSceneHeight());
	clear_to_color(pBitmap, g_MaskColor);
	m_pMOColorLayer = new SceneLayerTracked();
	m_pMOColorLayer->Create(pBitmap, true, Vector(), m_pCurrentScene->WrapsX(), m_pCurrentScene->WrapsY(), Vector(1.0, 1.0));
	pBitmap = 0;

	const int cellSize = 20;
	m_MOIDsGrid = SpatialPartitionGrid(GetSceneWidth(), GetSceneHeight(), cellSize);

	// Create the Debug SceneLayer
	if (m_DrawRayCastVisualizations || m_DrawPixelCheckVisualizations) {
		delete m_pDebugLayer;
		pBitmap = create_bitmap_ex(8, GetSceneWidth(), GetSceneHeight());
		clear_to_color(pBitmap, g_MaskColor);
		m_pDebugLayer = new SceneLayer();
		m_pDebugLayer->Create(pBitmap, true, Vector(), m_pCurrentScene->WrapsX(), m_pCurrentScene->WrapsY(), Vector(1.0, 1.0));
		pBitmap = nullptr;
	}

	// Finally draw the ID:s of the MO:s to the MOID layers for the first time
	g_MovableMan.UpdateDrawMOIDs();

	return 0;
}

int SceneMan::SetSceneToLoad(const std::string& sceneName, bool placeObjects, bool placeUnits) {
	// Use the name passed in to load the preset requested
	const Scene* pSceneRef = dynamic_cast<const Scene*>(g_PresetMan.GetEntityPreset("Scene", sceneName));

	if (!pSceneRef) {
		g_ConsoleMan.PrintString("ERROR: Finding Scene preset \'" + sceneName + "\' failed! Has it been properly defined?");
		return -1;
	}

	// Store the scene reference to load later
	SetSceneToLoad(pSceneRef, placeObjects, placeUnits);

	return 0;
}

int SceneMan::LoadScene() {
	// In case we have no set Scene reference to load from, do something graceful about it
	if (!m_pSceneToLoad) {
		// Try to use the Scene the current Activity is associated with
		if (g_ActivityMan.GetActivity())
			SetSceneToLoad(g_ActivityMan.GetActivity()->GetSceneName());

		// If that failed, then resort to the default scene name
		if (SetSceneToLoad(m_DefaultSceneName) < 0) {
			g_ConsoleMan.PrintString("ERROR: Couldn't start because no Scene has been specified to load!");
			return -1;
		}
	}

	return LoadScene(dynamic_cast<Scene*>(m_pSceneToLoad->Clone()), m_PlaceObjects, m_PlaceUnits);
}

int SceneMan::LoadScene(const std::string& sceneName, bool placeObjects, bool placeUnits) {
	// First retrieve and set up the preset reference
	int error = SetSceneToLoad(sceneName, placeObjects, placeUnits);
	if (error < 0)
		return error;
	// Now actually load and start it
	error = LoadScene();
	return error;
}

int SceneMan::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return Serializable::ReadProperty(propName, reader));

	MatchProperty("AddMaterial",
	              {
		              // Get this before reading Object, since if it's the last one in its datafile, the stream will show the parent file instead
		              std::string objectFilePath = reader.GetCurrentFilePath();

		              // Don't use the << operator, because it adds the material to the PresetMan before we get a chance to set the proper ID!
		              Material* pNewMat = new Material;
		              ((Serializable*)(pNewMat))->Create(reader);

		              // If the initially requested material slot is available, then put it there
		              // But if it's not available, then check if any subsequent one is, looping around the palette if necessary
		              for (int tryId = pNewMat->GetIndex(); tryId < c_PaletteEntriesNumber; ++tryId) {
			              // We found an empty slot in the Material palette!
			              if (m_apMatPalette.at(tryId) == nullptr) {
				              // If the final ID isn't the same as the one originally requested by the data file, then make the mapping so
				              // subsequent ID references to this within the same data module can be translated to the actual ID of this material
				              if (tryId != pNewMat->GetIndex())
					              g_PresetMan.AddMaterialMapping(pNewMat->GetIndex(), tryId, reader.GetReadModuleID());

				              // Assign the final ID to the material and register it in the palette
				              pNewMat->SetIndex(tryId);

				              // Ensure out-of-bounds material is unbreakable
				              if (tryId == MaterialColorKeys::g_MaterialOutOfBounds) {
					              RTEAssert(pNewMat->GetIntegrity() == std::numeric_limits<float>::max(), "Material with index " + std::to_string(MaterialColorKeys::g_MaterialOutOfBounds) + " (i.e out-of-bounds material) has a finite integrity!\n This should be infinity (-1).");
				              }

				              m_apMatPalette.at(tryId) = pNewMat;
				              m_MatNameMap.insert(std::pair<std::string, unsigned char>(std::string(pNewMat->GetPresetName()), pNewMat->GetIndex()));
				              // Now add the instance, when ID has been registered!
				              g_PresetMan.AddEntityPreset(pNewMat, reader.GetReadModuleID(), reader.GetPresetOverwriting(), objectFilePath);
				              ++m_MaterialCount;
				              break;
			              }
			              // We reached the end of the Material palette without finding any empty slots.. loop around to the start
			              else if (tryId >= c_PaletteEntriesNumber - 1)
				              tryId = 0;
			              // If we've looped around without finding anything, break and throw error
			              else if (tryId == pNewMat->GetIndex() - 1) {
				              // TODO: find the closest matching mateiral and map to it?
				              RTEAbort("Tried to load material \"" + pNewMat->GetPresetName() + "\" but the material palette (256 max) is full! Try consolidating or removing some redundant materials, or removing some entire data modules.");
				              break;
			              }
		              }
	              });

	EndPropertyList;
}

int SceneMan::Save(Writer& writer) const {
	g_ConsoleMan.PrintString("ERROR: Tried to save SceneMan, screen does not make sense");

	Serializable::Save(writer);

	for (int i = 0; i < m_MaterialCount; ++i) {
		writer.NewPropertyWithValue("AddMaterial", *(m_apMatPalette.at(i)));
	}

	return 0;
}

void SceneMan::Destroy() {
	for (int i = 0; i < c_PaletteEntriesNumber; ++i)
		delete m_apMatPalette[i];

	for (Material* materialCopy: m_MaterialCopiesVector) {
		delete materialCopy;
	}
	m_MaterialCopiesVector.clear();
	for (const auto& [index, copies]: m_RetiredMaterialCopies) for (Material* copy: copies) delete copy;
	for (auto& retired: m_RetiredPaletteMaterials) { for (Material* material: retired) delete material; retired.clear(); }
	m_RetiredMaterialCopies.clear();
	m_MaterialCopyIndices.clear();

	delete m_pCurrentScene;
	delete m_pDebugLayer;
	delete m_pMOColorLayer;
	delete m_pUnseenRevealSound;

	destroy_bitmap(m_pOrphanSearchBitmap);
	m_pOrphanSearchBitmap = 0;

	for (const auto& [bitmapSize, bitmapPtr]: m_IntermediateSettlingBitmaps) {
		destroy_bitmap(bitmapPtr);
	}

	Clear();
}

Vector SceneMan::GetSceneDim() const {
	if (m_pCurrentScene) {
		RTEAssert(m_pCurrentScene->GetTerrain() && m_pCurrentScene->GetTerrain()->GetBitmap(), "Trying to get terrain info before there is a scene or terrain!");
		return m_pCurrentScene->GetDimensions();
	}
	return Vector();
}

int SceneMan::GetSceneWidth() const {
	if (m_pCurrentScene)
		return m_pCurrentScene->GetWidth();
	return 0;
}

int SceneMan::GetSceneHeight() const {
	//    RTEAssert(m_pCurrentScene, "Trying to get terrain info before there is a scene or terrain!");
	if (m_pCurrentScene)
		return m_pCurrentScene->GetHeight();
	return 0;
}

bool SceneMan::SceneWrapsX() const {
	if (m_pCurrentScene)
		return m_pCurrentScene->WrapsX();
	return false;
}

bool SceneMan::SceneWrapsY() const {
	if (m_pCurrentScene)
		return m_pCurrentScene->WrapsY();
	return false;
}

Directions SceneMan::GetSceneOrbitDirection() const {
	if (m_pCurrentScene) {
		SLTerrain* terrain = m_pCurrentScene->GetTerrain();
		if (terrain) {
			return terrain->GetOrbitDirection();
		}
	}

	return Directions::Up;
}

SLTerrain* SceneMan::GetTerrain() {
	//    RTEAssert(m_pCurrentScene, "Trying to get terrain matter before there is a scene or terrain!");
	if (m_pCurrentScene) {
		return m_pCurrentScene->GetTerrain();
	}

	return nullptr;
}

BITMAP* SceneMan::GetMOColorBitmap() const {
	return m_pMOColorLayer->GetBitmap();
}

BITMAP* SceneMan::GetDebugBitmap() const {
	RTEAssert(m_pDebugLayer, "Tried to get debug bitmap but debug layer doesn't exist. Note that the debug layer is only created under certain circumstances.");
	return m_pDebugLayer->GetBitmap();
}

unsigned char SceneMan::GetTerrMatter(int pixelX, int pixelY) {
	RTEAssert(m_pCurrentScene, "Trying to get terrain matter before there is a scene or terrain!");

	WrapPosition(pixelX, pixelY);

	if (m_pDebugLayer && m_DrawPixelCheckVisualizations) {
		m_pDebugLayer->SetPixel(pixelX, pixelY, 5);
	}

	BITMAP* pTMatBitmap = m_pCurrentScene->GetTerrain()->GetMaterialBitmap();
	if (s_ReadTerrainFromCopy) {
		if (BITMAP* matCopy = m_pCurrentScene->GetTerrain()->GetMaterialCopyBitmap()) {
			pTMatBitmap = matCopy;
		}
	}
	if (pTMatBitmap == nullptr) {
		return g_MaterialAir;
	}

	// If it's still below or to the sides out of bounds after
	// what is supposed to be wrapped, shit is out of bounds.
	if (pixelX < 0 || pixelX >= pTMatBitmap->w || pixelY >= pTMatBitmap->h)
		return g_MaterialAir;

	// If above terrain bitmap, return air material.
	if (pixelY < 0)
		return g_MaterialAir;

	return getpixel(pTMatBitmap, pixelX, pixelY);
}

MOID SceneMan::GetMOIDPixel(int pixelX, int pixelY, int ignoreTeam) {
	WrapPosition(pixelX, pixelY);

	if (m_pDebugLayer && m_DrawPixelCheckVisualizations) {
		m_pDebugLayer->SetPixel(pixelX, pixelY, 5);
	}

	const std::vector<MOID>& moidList = m_MOIDsGrid.GetMOIDsAtPosition(pixelX, pixelY, ignoreTeam, true);
	MOID moid = g_MovableMan.GetMOIDPixel(pixelX, pixelY, moidList);

	return moid;
}

Material const* SceneMan::GetMaterial(const std::string& matName) {
	std::map<std::string, unsigned char>::iterator itr = m_MatNameMap.find(matName);
	if (itr == m_MatNameMap.end()) {
		g_ConsoleMan.PrintString("ERROR: Material of name: " + matName + " not found!");
		return 0;
	} else
		return m_apMatPalette.at((*itr).second);
}

Vector SceneMan::GetGlobalAcc() const {
	RTEAssert(m_pCurrentScene, "Trying to get terrain matter before there is a scene or terrain!");
	return m_pCurrentScene->GetGlobalAcc();
}

void SceneMan::RegisterDrawing(const BITMAP* bitmap, int moid, int left, int top, int right, int bottom) {
	if (m_pMOColorLayer && m_pMOColorLayer->GetBitmap() == bitmap) {
		m_pMOColorLayer->RegisterDrawing(left, top, right, bottom);
	} else if (m_RenderDrawContext) {
		// The MOID grid is sim state; render-frame draws happen at frame-timed positions and must not feed it.
		return;
	} else if (const MovableObject* mo = g_MovableMan.GetMOFromID(moid)) {
		IntRect rect(left, top, right, bottom);
		m_MOIDsGrid.Add(rect, *mo);
	}
}

void SceneMan::RegisterDrawing(const BITMAP* bitmap, int moid, const Vector& center, float radius) {
	if (radius != 0.0F) {
		RegisterDrawing(bitmap, moid, static_cast<int>(std::floor(center.m_X - radius)), static_cast<int>(std::floor(center.m_Y - radius)), static_cast<int>(std::floor(center.m_X + radius)), static_cast<int>(std::floor(center.m_Y + radius)));
	}
}

void SceneMan::ClearAllMOIDDrawings() {
	m_MOIDsGrid.Reset();
}

void SceneMan::FeedTerrainToSimChecksum() {
	// Heavy (full-bitmap) — only run it during a determinism trace, not normal play.
	if (!g_SimChecksum.IsActive() || !m_pCurrentScene) {
		return;
	}
	SLTerrain* terrain = m_pCurrentScene->GetTerrain();
	if (!terrain) {
		return;
	}
	HashTerrainBitmap(terrain->GetMaterialBitmap());
	HashTerrainBitmap(terrain->GetFGColorBitmap());
}

void SceneMan::HashTerrainBitmap(BITMAP* bitmap) {
	if (!bitmap) {
		return;
	}
	const int dims[2] = {bitmap->w, bitmap->h};
	g_SimChecksum.Update("terrain", dims, sizeof(dims));
	for (int y = 0; y < bitmap->h; ++y) {
		g_SimChecksum.Update("terrain", bitmap->line[y], static_cast<size_t>(bitmap->w));
	}
}

namespace {
	float PenetrationRetardation(float integrity, float squaredImpulse) {
		// Zero-sharpness particles can meet a zero-strength material, including an
		// undefined terrain index resolved to Air. That material has no resistance.
		if (integrity == 0.0F && squaredImpulse == 0.0F) return 0.0F;
		return -(integrity / std::sqrt(squaredImpulse));
	}

	// Feeds one penetration decision into the `carve_math` subsystem. kind: 0 = WillPenetrate,
	// 1 = TryPenetrate, 2 = DislodgePixel. High call volume — determinism-trace only.
	void FeedCarveMath(int kind, int posX, int posY, const Vector& impulse, const Vector& velocity,
	                   int materialID, bool result, float retardation) {
		if (!g_SimChecksum.IsActive()) {
			return;
		}
		auto floatBits = [](float f) -> int64_t {
			uint32_t u = 0;
			std::memcpy(&u, &f, sizeof(u));
			return static_cast<int64_t>(u);
		};
		const int64_t fields[] = {
		    kind, posX, posY, materialID, result ? 1 : 0,
		    floatBits(impulse.m_X), floatBits(impulse.m_Y),
		    floatBits(velocity.m_X), floatBits(velocity.m_Y),
		    floatBits(retardation),
		};
		g_SimChecksum.Update("carve_math", fields, sizeof(fields));
	}
} // namespace

bool SceneMan::WillPenetrate(const int posX,
                             const int posY,
                             const Vector& impulse) {
	RTEAssert(m_pCurrentScene, "Trying to access scene before there is one!");

	if (!m_pCurrentScene->GetTerrain()->IsWithinBounds(posX, posY))
		return false;

	unsigned char materialID = getpixel(m_pCurrentScene->GetTerrain()->GetMaterialBitmap(), posX, posY);
	float integrity = GetMaterialFromID(materialID)->GetIntegrity();
	const bool result = impulse.MagnitudeIsGreaterThan(integrity);
	FeedCarveMath(0, posX, posY, impulse, Vector(), materialID, result, 0.0F);
	return result;
}

int SceneMan::RemoveOrphans(int posX, int posY, int radius, int maxArea, bool remove) {
	if (radius > MAXORPHANRADIUS)
		radius = MAXORPHANRADIUS;

	clear_to_color(m_pOrphanSearchBitmap, g_MaterialAir);
	int area = RemoveOrphans(posX, posY, posX, posY, 0, radius, maxArea, false);
	if (remove && area <= maxArea) {
		clear_to_color(m_pOrphanSearchBitmap, g_MaterialAir);
		RemoveOrphans(posX, posY, posX, posY, 0, radius, maxArea, true);
	}

	return area;
}

int SceneMan::RemoveOrphans(int posX, int posY,
                            int centerPosX, int centerPosY,
                            int accumulatedArea, int radius, int maxArea, bool remove) {
	int area = 0;
	int bmpX = 0;
	int bmpY = 0;

	BITMAP* mat = m_pCurrentScene->GetTerrain()->GetMaterialBitmap();

	if (posX < 0 || posY < 0 || posX >= mat->w || posY >= mat->h)
		return 0;

	unsigned char materialID = _getpixel(mat, posX, posY);
	if (materialID == g_MaterialAir && (posX != centerPosX || posY != centerPosY))
		return 0;
	else {
		bmpX = posX - (centerPosX - radius / 2);
		bmpY = posY - (centerPosY - radius / 2);

		// We reached the border of orphan-searching area and
		// there are still material pixels there -> the area is not an orphaned teran piece, abort search
		if (bmpX <= 0 || bmpY <= 0 || bmpX >= radius - 1 || bmpY >= radius - 1)
			return MAXORPHANRADIUS * MAXORPHANRADIUS + 1;
		else
		// Check if pixel was already checked
		{
			if (_getpixel(m_pOrphanSearchBitmap, bmpX, bmpY) != g_MaterialAir)
				return 0;
		}
	}

	_putpixel(m_pOrphanSearchBitmap, bmpX, bmpY, materialID);
	area++;

	// We're clear to remove the pixel
	if (remove) {
		Material const* sceneMat = GetMaterialFromID(materialID);
		Material const* spawnMat;
		spawnMat = sceneMat->GetSpawnMaterial() ? GetMaterialFromID(sceneMat->GetSpawnMaterial()) : sceneMat;
		float sprayScale = 0.1;
		Color spawnColor;
		if (spawnMat->UsesOwnColor())
			spawnColor = spawnMat->GetColor();
		else
			spawnColor.SetRGBWithIndex(m_pCurrentScene->GetTerrain()->GetFGColorPixel(posX, posY));

		// No point generating a key-colored MOPixel
		if (spawnColor.GetIndex() != g_MaskColor) {
			// TEST COLOR
			// spawnColor = 5;

			// Get the new pixel from the pre-allocated pool, should be faster than dynamic allocation
			// Density is used as the mass for the new MOPixel
			float tempMax = 2.0F * sprayScale;
			float tempMin = tempMax / 2.0F;
			// Order the draws explicitly — unsequenced arg evaluation desyncs cross-compiler.
			const float orphanVelX = -RandomNum(tempMin, tempMax);
			const float orphanVelY = -RandomNum(tempMin, tempMax);
			MOPixel* pixelMO = new MOPixel(spawnColor,
			                               spawnMat->GetPixelDensity(),
			                               Vector(posX, posY),
			                               Vector(orphanVelX, orphanVelY),
			                               new Atom(Vector(), spawnMat->GetIndex(), 0, spawnColor, 2),
			                               0);

			pixelMO->SetToHitMOs(spawnMat->GetIndex() == c_GoldMaterialID);
			pixelMO->SetToGetHitByMOs(false);
			g_MovableMan.AddParticle(pixelMO);
			pixelMO = 0;
		}
		TraceTerrainEvent("orph", posX, posY, materialID, 0, static_cast<int>(s_TerrainEventContextUID));
		m_pCurrentScene->GetTerrain()->SetFGColorPixel(posX, posY, g_MaskColor);
		m_pCurrentScene->GetTerrain()->SetMaterialPixel(posX, posY, g_MaterialAir);
	}

	int xoff[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
	int yoff[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

	for (int c = 0; c < 8; c++) {
		area += RemoveOrphans(posX + xoff[c], posY + yoff[c], centerPosX, centerPosY, area, radius, maxArea, remove);
		if (accumulatedArea + area > maxArea)
			break;
	}

	return area;
}

bool SceneMan::TryPenetrate(int posX,
                            int posY,
                            const Vector& impulse,
                            const Vector& velocity,
                            float& retardation,
                            const float airRatio,
                            const int numPenetrations,
                            const int removeOrphansRadius,
                            const int removeOrphansMaxArea,
                            const float removeOrphansRate) {
	RTEAssert(m_pCurrentScene, "Trying to access scene before there is one!");

	if (!m_pCurrentScene->GetTerrain()->IsWithinBounds(posX, posY))
		return false;

	WrapPosition(posX, posY);
	unsigned char materialID = _getpixel(m_pCurrentScene->GetTerrain()->GetMaterialBitmap(), posX, posY);
	if (materialID == g_MaterialAir) {
		//        RTEAbort("Why are we penetrating air??");
		return true;
	}
	Material const* sceneMat = GetMaterialFromID(materialID);
	Material const* spawnMat;

	float sprayScale = 0.1F;
	float sqrImpMag = impulse.GetSqrMagnitude();
	const bool tracePenetration = IsTrackedUID(s_TerrainEventContextUID);
	if (tracePenetration) {
		const auto bits = [](float value) { return std::bit_cast<int32_t>(value); };
		TraceTerrainEvent("pimp", bits(impulse.m_X), bits(impulse.m_Y), bits(velocity.m_X), bits(velocity.m_Y), static_cast<int>(s_TerrainEventContextUID));
		TraceTerrainEvent("pmat", posX, posY, materialID, sceneMat->GetIndex(), static_cast<int>(s_TerrainEventContextUID));
		TraceTerrainEvent("pmag", bits(sqrImpMag), bits(sceneMat->GetIntegrity()), bits(sceneMat->GetIntegrity() * sceneMat->GetIntegrity()), 0, static_cast<int>(s_TerrainEventContextUID));
	}

	// Test if impulse force is enough to penetrate
	if (sqrImpMag >= (sceneMat->GetIntegrity() * sceneMat->GetIntegrity())) {
		if (numPenetrations <= 3) {
			spawnMat = sceneMat->GetSpawnMaterial() ? GetMaterialFromID(sceneMat->GetSpawnMaterial()) : sceneMat;
			Color spawnColor;
			if (spawnMat->UsesOwnColor())
				spawnColor = spawnMat->GetColor();
			else
				spawnColor.SetRGBWithIndex(m_pCurrentScene->GetTerrain()->GetFGColorPixel(posX, posY));

			// No point generating a key-colored MOPixel
			if (spawnColor.GetIndex() != g_MaskColor) {
				// Get the new pixel from the pre-allocated pool, should be faster than dynamic allocation
				// Density is used as the mass for the new MOPixel
				/*                MOPixel *pixelMO = dynamic_cast<MOPixel *>(MOPixel::InstanceFromPool());
				                pixelMO->Create(spawnColor,
				                                spawnMat.pixelDensity,
				                                Vector(posX, posY),
				                                Vector(-RandomNum((velocity.m_X * sprayScale) / 2 , velocity.m_X * sprayScale),
				                                       -RandomNum((velocity.m_Y * sprayScale) / 2 , velocity.m_Y * sprayScale)),
				//                                               -(impulse * (sprayScale * RandomNum() / spawnMat.density)),
				                                new Atom(Vector(), spawnMat, 0, spawnColor, 2),
				                                0);
				*/
				float tempMaxX = velocity.m_X * sprayScale;
				float tempMinX = tempMaxX / 2.0F;
				float tempMaxY = velocity.m_Y * sprayScale;
				float tempMinY = tempMaxY / 2.0F;
				// Order the draws explicitly — unsequenced arg evaluation desyncs cross-compiler.
				const float sprayVelX = -RandomNum(tempMinX, tempMaxX);
				const float sprayVelY = -RandomNum(tempMinY, tempMaxY);
				MOPixel* pixelMO = new MOPixel(spawnColor,
				                               spawnMat->GetPixelDensity(),
				                               Vector(posX, posY),
				                               Vector(sprayVelX, sprayVelY),
				                               //                                              -(impulse * (sprayScale * RandomNum() / spawnMat.density)),
				                               new Atom(Vector(), spawnMat->GetIndex(), 0, spawnColor, 2),
				                               0);

				// TODO: Make material IDs more robust!")
				pixelMO->SetToHitMOs(spawnMat->GetIndex() == c_GoldMaterialID);
				pixelMO->SetToGetHitByMOs(false);
				g_MovableMan.AddParticle(pixelMO);
				pixelMO = 0;
			}
			TraceTerrainEvent("dis", posX, posY, materialID, 0, static_cast<int>(s_TerrainEventContextUID));
			m_pCurrentScene->GetTerrain()->SetFGColorPixel(posX, posY, g_MaskColor);
			m_pCurrentScene->GetTerrain()->SetMaterialPixel(posX, posY, g_MaterialAir);
		}
		// TODO: Improve / tweak randomized pushing away of terrain")
		else if (RandomNum() <= airRatio) {
			TraceTerrainEvent("disa", posX, posY, materialID, 0, static_cast<int>(s_TerrainEventContextUID));
			m_pCurrentScene->GetTerrain()->SetFGColorPixel(posX, posY, g_MaskColor);
			m_pCurrentScene->GetTerrain()->SetMaterialPixel(posX, posY, g_MaterialAir);
		}

		// Save the impulse force effects of the penetrating particle.
		//        retardation = -sceneMat.density;
		retardation = PenetrationRetardation(sceneMat->GetIntegrity(), sqrImpMag);
		if (tracePenetration) {
			const auto bits = [](float value) { return std::bit_cast<int32_t>(value); };
			TraceTerrainEvent("pres", bits(std::sqrt(sqrImpMag)), bits(retardation), bits(sceneMat->GetIntegrity()), bits(sqrImpMag), static_cast<int>(s_TerrainEventContextUID));
		}

		// If this is a scrap pixel, or there is no background pixel 'supporting' the knocked-loose pixel, make the column above also turn into particles.
		if (m_ScrapCompactingHeight > 0 && (sceneMat->IsScrap() || _getpixel(m_pCurrentScene->GetTerrain()->GetBGColorBitmap(), posX, posY) == g_MaskColor)) {
			// Get quicker direct access to bitmaps
			BITMAP* pFGColor = m_pCurrentScene->GetTerrain()->GetFGColorBitmap();
			BITMAP* pBGColor = m_pCurrentScene->GetTerrain()->GetBGColorBitmap();
			BITMAP* pMaterial = m_pCurrentScene->GetTerrain()->GetMaterialBitmap();

			int testMaterialID = g_MaterialAir;
			MOPixel* pixelMO = 0;
			Color spawnColor;
			float sprayMag = std::sqrt(velocity.GetMagnitude() * sprayScale);
			Vector sprayVel;

			for (int testY = posY - 1; testY > posY - m_ScrapCompactingHeight && testY >= 0; --testY) {
				if ((testMaterialID = _getpixel(pMaterial, posX, testY)) != g_MaterialAir) {
					sceneMat = GetMaterialFromID(testMaterialID);

					if (sceneMat->IsScrap() || _getpixel(pBGColor, posX, testY) == g_MaskColor) {
						if (RandomNum() < 0.7F) {
							spawnMat = sceneMat->GetSpawnMaterial() ? GetMaterialFromID(sceneMat->GetSpawnMaterial()) : sceneMat;
							if (spawnMat->UsesOwnColor()) {
								spawnColor = spawnMat->GetColor();
							} else {
								spawnColor.SetRGBWithIndex(m_pCurrentScene->GetTerrain()->GetFGColorPixel(posX, testY));
							}
							if (spawnColor.GetIndex() != g_MaskColor) {
								// Send terrain pixels flying at a diminishing rate the higher the column goes.
								sprayVel.SetXY(0, -sprayMag * (1.0F - (static_cast<float>(posY - testY) / static_cast<float>(m_ScrapCompactingHeight))));
								sprayVel.RadRotate(RandomNum(-c_HalfPI, c_HalfPI));

								pixelMO = new MOPixel(spawnColor, spawnMat->GetPixelDensity(), Vector(posX, testY), sprayVel, new Atom(Vector(), spawnMat->GetIndex(), 0, spawnColor, 2), 0);

								pixelMO->SetToHitMOs(spawnMat->GetIndex() == c_GoldMaterialID);
								pixelMO->SetToGetHitByMOs(false);
								g_MovableMan.AddParticle(pixelMO);
								pixelMO = 0;
							}
							RemoveOrphans(posX + testY % 2 ? -1 : 1, testY, removeOrphansRadius + 5, removeOrphansMaxArea + 10, true);
						}
						TraceTerrainEvent("disc", posX, testY, testMaterialID, 0, static_cast<int>(s_TerrainEventContextUID));
						_putpixel(pFGColor, posX, testY, g_MaskColor);
						_putpixel(pMaterial, posX, testY, g_MaterialAir);
					} else {
						break;
					}
				}
			}
		}

		// Remove orphaned regions if told to by parent MO who travelled an atom which tries to penetrate terrain
		if (removeOrphansRadius && removeOrphansMaxArea && removeOrphansRate > 0 && RandomNum() < removeOrphansRate) {
			RemoveOrphans(posX, posY, removeOrphansRadius, removeOrphansMaxArea, true);
			/*PALETTE palette;
			get_palette(palette);
			save_bmp("Orphan.bmp", m_pOrphanSearchBitmap, palette);*/
		}

		FeedCarveMath(1, posX, posY, impulse, velocity, materialID, true, retardation);
		return true;
	}
	FeedCarveMath(1, posX, posY, impulse, velocity, materialID, false, 0.0F);
	return false;
}

MOPixel* SceneMan::DislodgePixel(int posX, int posY) {
	WrapPosition(posX, posY);
	int materialID = getpixel(m_pCurrentScene->GetTerrain()->GetMaterialBitmap(), posX, posY);
	if (materialID <= MaterialColorKeys::g_MaterialAir) {
		return nullptr;
	}
	const Material* sceneMat = GetMaterialFromID(static_cast<uint8_t>(materialID));
	const Material* spawnMat = sceneMat->GetSpawnMaterial() ? GetMaterialFromID(sceneMat->GetSpawnMaterial()) : sceneMat;

	Color spawnColor;
	if (spawnMat->UsesOwnColor()) {
		spawnColor = spawnMat->GetColor();
	} else {
		spawnColor.SetRGBWithIndex(m_pCurrentScene->GetTerrain()->GetFGColorPixel(posX, posY));
	}
	// No point generating a key-colored MOPixel.
	if (spawnColor.GetIndex() == ColorKeys::g_MaskColor) {
		return nullptr;
	}
	Atom* pixelAtom = new Atom(Vector(), spawnMat->GetIndex(), nullptr, spawnColor, 2);
	MOPixel* pixelMO = new MOPixel(spawnColor, spawnMat->GetPixelDensity(), Vector(static_cast<float>(posX), static_cast<float>(posY)), Vector(), pixelAtom, 0);
	pixelMO->SetToHitMOs(spawnMat->GetIndex() == c_GoldMaterialID);
	TraceTerrainEvent("disp", posX, posY, materialID, 0, static_cast<int>(s_TerrainEventContextUID));
	g_MovableMan.AddParticle(pixelMO);

	m_pCurrentScene->GetTerrain()->SetFGColorPixel(posX, posY, ColorKeys::g_MaskColor);
	m_pCurrentScene->GetTerrain()->SetMaterialPixel(posX, posY, MaterialColorKeys::g_MaterialAir);

	FeedCarveMath(2, posX, posY, Vector(), Vector(), materialID, true, 0.0F);
	return pixelMO;
}

// Bool variant to avoid changing the original
MOPixel* SceneMan::DislodgePixelBool(int posX, int posY, bool deletePixel) {
	MOPixel* pixelMO = DislodgePixel(posX, posY);
	if (pixelMO) {
		pixelMO->SetToDelete(deletePixel);
	}
	return pixelMO;
}

std::vector<MOPixel*>* SceneMan::DislodgePixelCircle(const Vector& centre, float radius, bool deletePixels) {
	std::vector<MOPixel*>* pixelList = new std::vector<MOPixel*>();
	int limit = static_cast<int>(radius) * 2;
	for (int x = 0; x <= limit; x++) {
		for (int y = 0; y <= limit; y++) {
			Vector checkPos = Vector(static_cast<float>(x) - radius, static_cast<float>(y) - radius) + centre;
			Vector distance = ShortestDistance(centre, checkPos, true);

			if (distance.MagnitudeIsGreaterThan(radius) && y > limit / 2) {
				break;
			}

			if (!distance.MagnitudeIsGreaterThan(radius)) {
				MOPixel* px = DislodgePixelBool(checkPos.m_X, checkPos.m_Y, deletePixels);
				if (px) {
					pixelList->push_back(px);
				}
			}
		}
	}

	return pixelList;
}

std::vector<MOPixel*>* SceneMan::DislodgePixelCircleNoBool(const Vector& centre, float radius) {
	return DislodgePixelCircle(centre, radius, false);
}

std::vector<MOPixel*>* SceneMan::DislodgePixelRing(const Vector& centre, float innerRadius, float outerRadius, bool deletePixels) {
	// Account for users inputting radii in the wrong order
	if (outerRadius < innerRadius) {
		std::swap(outerRadius, innerRadius);
	}

	std::vector<MOPixel*>* pixelList = new std::vector<MOPixel*>();
	int limit = static_cast<int>(outerRadius) * 2;
	for (int x = 0; x <= limit; x++) {
		for (int y = 0; y <= limit; y++) {
			Vector checkPos = Vector(static_cast<float>(x) - outerRadius, static_cast<float>(y) - outerRadius) + centre;
			Vector distance = ShortestDistance(centre, checkPos, true);

			if (distance.MagnitudeIsLessThan(innerRadius) && y < limit - y) {
				y = limit - y;
				continue;
			}

			if (distance.MagnitudeIsGreaterThan(outerRadius) && y > limit / 2) {
				break;
			}

			if (!distance.MagnitudeIsGreaterThan(outerRadius) && !distance.MagnitudeIsLessThan(innerRadius)) {
				MOPixel* px = DislodgePixelBool(checkPos.m_X, checkPos.m_Y, deletePixels);
				if (px) {
					pixelList->push_back(px);
				}
			}
		}
	}

	return pixelList;
}

std::vector<MOPixel*>* SceneMan::DislodgePixelRingNoBool(const Vector& centre, float innerRadius, float outerRadius) {
	return DislodgePixelRing(centre, innerRadius, outerRadius, false);
}

std::vector<MOPixel*>* SceneMan::DislodgePixelBox(const Vector& upperLeftCorner, const Vector& lowerRightCorner, bool deletePixels) {
	std::vector<MOPixel*>* pixelList = new std::vector<MOPixel*>();

	// Make sure it works even if people input corners in the wrong order
	Vector start = Vector(std::min(upperLeftCorner.m_X, lowerRightCorner.m_X), std::min(upperLeftCorner.m_Y, lowerRightCorner.m_Y));
	Vector end = Vector(std::max(upperLeftCorner.m_X, lowerRightCorner.m_X), std::max(upperLeftCorner.m_Y, lowerRightCorner.m_Y));

	float width = end.m_X - start.m_X;
	float height = end.m_Y - start.m_Y;
	for (int x = 0; x <= static_cast<int>(width) * 2; x++) {
		for (int y = 0; y <= static_cast<int>(height) * 2; y++) {
			Vector checkPos = start + Vector(static_cast<float>(x), static_cast<float>(y));
			MOPixel* px = DislodgePixelBool(checkPos.m_X, checkPos.m_Y, deletePixels);
			if (px) {
				pixelList->push_back(px);
			}
		}
	}

	return pixelList;
}

std::vector<MOPixel*>* SceneMan::DislodgePixelBoxNoBool(const Vector& upperLeftCorner, const Vector& lowerRightCorner) {
	return DislodgePixelBox(upperLeftCorner, lowerRightCorner, false);
}

std::vector<MOPixel*>* SceneMan::DislodgePixelLine(const Vector& start, const Vector& ray, int skip, bool deletePixels) {
	std::vector<MOPixel*>* pixelList = new std::vector<MOPixel*>();
	int error, dom, sub, domSteps, skipped = skip;
	int intPos[2], delta[2], delta2[2], increment[2];

	intPos[X] = std::floor(start.m_X);
	intPos[Y] = std::floor(start.m_Y);
	delta[X] = std::floor(start.m_X + ray.m_X) - intPos[X];
	delta[Y] = std::floor(start.m_Y + ray.m_Y) - intPos[Y];

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm preparation

	if (delta[X] < 0) {
		increment[X] = -1;
		delta[X] = -delta[X];
	} else
		increment[X] = 1;

	if (delta[Y] < 0) {
		increment[Y] = -1;
		delta[Y] = -delta[Y];
	} else
		increment[Y] = 1;

	// Scale by 2, for better accuracy of the error at the first pixel
	delta2[X] = delta[X] << 1;
	delta2[Y] = delta[Y] << 1;

	// If X is dominant, Y is submissive, and vice versa.
	if (delta[X] > delta[Y]) {
		dom = X;
		sub = Y;
	} else {
		dom = Y;
		sub = X;
	}

	error = delta2[sub] - delta[dom];

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm execution

	for (domSteps = 0; domSteps < delta[dom]; ++domSteps) {
		intPos[dom] += increment[dom];
		if (error >= 0) {
			intPos[sub] += increment[sub];
			error -= delta2[dom];
		}
		error += delta2[sub];

		// Only check pixel if we're not due to skip any, or if this is the last pixel
		if (++skipped > skip || domSteps + 1 == delta[dom]) {
			// Scene wrapping
			g_SceneMan.WrapPosition(intPos[X], intPos[Y]);

			MOPixel* px = DislodgePixelBool(intPos[X], intPos[Y], deletePixels);
			if (px) {
				pixelList->push_back(px);
			}

			// Reset skip counter
			skipped = 0;
		}
	}

	return pixelList;
}

std::vector<MOPixel*>* SceneMan::DislodgePixelLineNoBool(const Vector& start, const Vector& ray, int skip) {
	return DislodgePixelLine(start, ray, skip, false);
}

void SceneMan::MakeAllUnseen(Vector pixelSize, const int team) {
	RTEAssert(m_pCurrentScene, "Messing with scene before the scene exists!");
	if (team < Activity::TeamOne || team >= Activity::MaxTeamCount)
		return;

	m_pCurrentScene->FillUnseenLayer(pixelSize, team);
}

bool SceneMan::LoadUnseenLayer(const std::string& bitmapPath, int team) {
	ContentFile bitmapFile(bitmapPath.c_str());
	SceneLayer* pUnseenLayer = new SceneLayer();
	if (pUnseenLayer->Create(bitmapFile.GetAsBitmap(COLORCONV_NONE, false), true, Vector(), m_pCurrentScene->WrapsX(), m_pCurrentScene->WrapsY(), Vector(1.0, 1.0)) < 0) {
		g_ConsoleMan.PrintString("ERROR: Loading background layer " + pUnseenLayer->GetPresetName() + "\'s data failed!");
		return false;
	}

	// Pass in ownership here
	m_pCurrentScene->SetUnseenLayer(pUnseenLayer, team);
	return true;
}

bool SceneMan::AnythingUnseen(const int team) {
	RTEAssert(m_pCurrentScene, "Checking scene before the scene exists when checking if anything is unseen!");

	return m_pCurrentScene->GetUnseenLayer(team) != 0;
	// TODO: Actually check all pixels on the map too?
}

Vector SceneMan::GetUnseenResolution(const int team) const {
	RTEAssert(m_pCurrentScene, "Checking scene before the scene exists when getting unseen resolution!");
	if (team < Activity::TeamOne || team >= Activity::MaxTeamCount)
		return Vector(1, 1);

	SceneLayer* pUnseenLayer = m_pCurrentScene->GetUnseenLayer(team);
	if (pUnseenLayer)
		return pUnseenLayer->GetScaleFactor();

	return Vector(1, 1);
}

bool SceneMan::IsUnseen(const int posX, const int posY, const int team) {
	RTEAssert(m_pCurrentScene, "Checking scene before the scene exists when checking if a position is unseen!");
	if (team < Activity::TeamOne || team >= Activity::MaxTeamCount)
		return false;

	SceneLayer* pUnseenLayer = m_pCurrentScene->GetUnseenLayer(team);
	if (pUnseenLayer) {
		// Translate to the scaled unseen layer's coordinates
		Vector scale = pUnseenLayer->GetScaleFactor();
		int scaledX = posX / scale.m_X;
		int scaledY = posY / scale.m_Y;
		return getpixel(pUnseenLayer->GetBitmap(), scaledX, scaledY) != g_MaskColor;
	}

	return false;
}

bool SceneMan::RevealUnseen(const int posX, const int posY, const int team) {
	RTEAssert(m_pCurrentScene, "Checking scene before the scene exists when revealing an unseen position!");
	if (team < Activity::TeamOne || team >= Activity::MaxTeamCount)
		return false;

	SceneLayer* pUnseenLayer = m_pCurrentScene->GetUnseenLayer(team);
	if (pUnseenLayer) {
		// Translate to the scaled unseen layer's coordinates
		Vector scale = pUnseenLayer->GetScaleFactor();
		int scaledX = posX / scale.m_X;
		int scaledY = posY / scale.m_Y;

		// Make sure we're actually revealing an unseen pixel that is ON the bitmap!
		int pixel = getpixel(pUnseenLayer->GetBitmap(), scaledX, scaledY);
		if (pixel != g_MaskColor && pixel != -1) {
			// Add the pixel to the list of now seen pixels so it can be visually flashed
			m_pCurrentScene->GetSeenPixels(team).push_back(Vector(scaledX, scaledY));
			// Clear to key color that pixel on the map so it won't be detected as unseen again
			putpixel(pUnseenLayer->GetBitmap(), scaledX, scaledY, g_MaskColor);
			// Play the reveal sound, if there's not too many already revealed this frame
			if (g_SettingsMan.BlipOnRevealUnseen() && m_pUnseenRevealSound && m_pCurrentScene->GetSeenPixels(team).size() < 5)
				m_pUnseenRevealSound->Play(Vector(posX, posY));
			// Show that we actually cleared an unseen pixel
			return true;
		}
	}

	return false;
}

bool SceneMan::RestoreUnseen(const int posX, const int posY, const int team) {
	RTEAssert(m_pCurrentScene, "Checking scene before the scene exists when making a position unseen!");
	if (team < Activity::TeamOne || team >= Activity::MaxTeamCount)
		return false;

	SceneLayer* pUnseenLayer = m_pCurrentScene->GetUnseenLayer(team);
	if (pUnseenLayer) {
		// Translate to the scaled unseen layer's coordinates
		Vector scale = pUnseenLayer->GetScaleFactor();
		int scaledX = posX / scale.m_X;
		int scaledY = posY / scale.m_Y;

		// Make sure we're actually hiding a seen pixel that is ON the bitmap!
		int pixel = getpixel(pUnseenLayer->GetBitmap(), scaledX, scaledY);
		if (pixel != g_BlackColor && pixel != -1) {
			// Restore that pixel on the map so it won't be detected as seen again
			putpixel(pUnseenLayer->GetBitmap(), scaledX, scaledY, g_BlackColor);
			// Show that we actually restored a seen pixel
			return true;
		}
	}

	return false;
}

void SceneMan::RevealUnseenBox(const int posX, const int posY, const int width, const int height, const int team) {
	RTEAssert(m_pCurrentScene, "Checking scene before the scene exists when revealing an unseen area!");
	if (team < Activity::TeamOne || team >= Activity::MaxTeamCount)
		return;

	SceneLayer* pUnseenLayer = m_pCurrentScene->GetUnseenLayer(team);
	if (pUnseenLayer) {
		// Translate to the scaled unseen layer's coordinates
		Vector scale = pUnseenLayer->GetScaleFactor();
		int scaledX = posX / scale.m_X;
		int scaledY = posY / scale.m_Y;
		int scaledW = width / scale.m_X;
		int scaledH = height / scale.m_Y;

		// Fill the box
		rectfill(pUnseenLayer->GetBitmap(), scaledX, scaledY, scaledX + scaledW, scaledY + scaledH, g_MaskColor);
	}
}

void SceneMan::RestoreUnseenBox(const int posX, const int posY, const int width, const int height, const int team) {
	RTEAssert(m_pCurrentScene, "Checking scene before the scene exists when making an area unseen!");
	if (team < Activity::TeamOne || team >= Activity::MaxTeamCount)
		return;

	SceneLayer* pUnseenLayer = m_pCurrentScene->GetUnseenLayer(team);
	if (pUnseenLayer) {
		// Translate to the scaled unseen layer's coordinates
		Vector scale = pUnseenLayer->GetScaleFactor();
		int scaledX = posX / scale.m_X;
		int scaledY = posY / scale.m_Y;
		int scaledW = width / scale.m_X;
		int scaledH = height / scale.m_Y;

		// Fill the box
		rectfill(pUnseenLayer->GetBitmap(), scaledX, scaledY, scaledX + scaledW, scaledY + scaledH, g_BlackColor);
	}
}

bool SceneMan::CastTerrainPenetrationRay(const Vector& start, const Vector& ray, Vector& endPos, int strengthLimit, int skip) {
	int error, dom, sub, domSteps, skipped = skip;
	int intPos[2], delta[2], delta2[2], increment[2];
	bool stopped = false;
	unsigned char materialID;
	Material const* foundMaterial;
	int totalStrength = 0;
	// Save the projected end of the ray pos
	endPos = start + ray;

	intPos[X] = std::floor(start.m_X);
	intPos[Y] = std::floor(start.m_Y);
	delta[X] = std::floor(start.m_X + ray.m_X) - intPos[X];
	delta[Y] = std::floor(start.m_Y + ray.m_Y) - intPos[Y];

	if (delta[X] == 0 && delta[Y] == 0)
		return false;

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm preparation

	if (delta[X] < 0) {
		increment[X] = -1;
		delta[X] = -delta[X];
	} else
		increment[X] = 1;

	if (delta[Y] < 0) {
		increment[Y] = -1;
		delta[Y] = -delta[Y];
	} else
		increment[Y] = 1;

	// Scale by 2, for better accuracy of the error at the first pixel
	delta2[X] = delta[X] << 1;
	delta2[Y] = delta[Y] << 1;

	// If X is dominant, Y is submissive, and vice versa.
	if (delta[X] > delta[Y]) {
		dom = X;
		sub = Y;
	} else {
		dom = Y;
		sub = X;
	}

	error = delta2[sub] - delta[dom];

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm execution

	for (domSteps = 0; domSteps < delta[dom]; ++domSteps) {
		intPos[dom] += increment[dom];
		if (error >= 0) {
			intPos[sub] += increment[sub];
			error -= delta2[dom];
		}
		error += delta2[sub];

		// Only check pixel if we're not due to skip any, or if this is the last pixel
		if (++skipped > skip || domSteps + 1 == delta[dom]) {
			// Scene wrapping
			g_SceneMan.WrapPosition(intPos[X], intPos[Y]);

			// Check the strength of the terrain to see if we can penetrate further
			materialID = GetTerrMatter(intPos[X], intPos[Y]);
			// Get the material object
			foundMaterial = GetMaterialFromID(materialID);
			// Add the encountered material's strength to the tally
			totalStrength += foundMaterial->GetIntegrity();
			// See if we have hit the limits of our ray's strength
			if (totalStrength >= strengthLimit) {
				// Save the position of the end of the ray where blocked
				endPos.SetXY(intPos[X], intPos[Y]);
				stopped = true;
				break;
			}
			// Reset skip counter
			skipped = 0;
			if (m_pDebugLayer && m_DrawRayCastVisualizations) {
				m_pDebugLayer->SetPixel(intPos[X], intPos[Y], 13);
			}
		}
	}

	return stopped;
}

// TODO Every raycast should use some shared line drawing method (or maybe something more efficient if it exists, that needs looking into) instead of having a ton of duplicated code.
bool SceneMan::CastUnseenRay(int team, const Vector& start, const Vector& ray, Vector& endPos, int strengthLimit, int skip, bool reveal) {
	if (!m_pCurrentScene->GetUnseenLayer(team))
		return false;

	int error, dom, sub, domSteps, skipped = skip;
	int size = 40 - GetUnseenResolution(team).GetLargest();
	int intPos[2], delta[2], delta2[2], increment[2];
	bool affectedAny = false;
	unsigned char materialID;
	Material const* foundMaterial;
	int totalStrength = 0;
	// Save the projected end of the ray pos
	endPos = start + ray;

	intPos[X] = std::floor(start.m_X);
	intPos[Y] = std::floor(start.m_Y);
	delta[X] = std::floor(start.m_X + ray.m_X) - intPos[X];
	delta[Y] = std::floor(start.m_Y + ray.m_Y) - intPos[Y];

	if (delta[X] == 0 && delta[Y] == 0)
		return false;

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm preparation

	if (delta[X] < 0) {
		increment[X] = -1;
		delta[X] = -delta[X];
	} else
		increment[X] = 1;

	if (delta[Y] < 0) {
		increment[Y] = -1;
		delta[Y] = -delta[Y];
	} else
		increment[Y] = 1;

	// Scale by 2, for better accuracy of the error at the first pixel
	delta2[X] = delta[X] << 1;
	delta2[Y] = delta[Y] << 1;

	// If X is dominant, Y is submissive, and vice versa.
	if (delta[X] > delta[Y]) {
		dom = X;
		sub = Y;
	} else {
		dom = Y;
		sub = X;
	}

	error = delta2[sub] - delta[dom];

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm execution

	for (domSteps = 0; domSteps < delta[dom]; ++domSteps) {
		intPos[dom] += increment[dom];
		if (error >= 0) {
			intPos[sub] += increment[sub];
			error -= delta2[dom];
		}
		error += delta2[sub];

		// Only check space if we're not due to skip any, or if this is the last step
		if (++skipped > skip || domSteps + 1 == delta[dom]) {
			// Scene wrapping
			WrapPosition(intPos[X], intPos[Y]);

			bool is_unseen = IsUnseen(intPos[X], intPos[Y], team) || IsUnseen(intPos[X] - size, intPos[Y] - size, team) || IsUnseen(intPos[X] + size, intPos[Y] - size, team) || IsUnseen(intPos[X] + size, intPos[Y] + size, team) || IsUnseen(intPos[X] - size, intPos[Y] + size, team) || IsUnseen(intPos[X] - size, intPos[Y], team) || IsUnseen(intPos[X] + size, intPos[Y], team) || IsUnseen(intPos[X], intPos[Y] - size, team) || IsUnseen(intPos[X], intPos[Y] + size, team);

			// Reveal if we can, save the result
			if (reveal) {
				if (is_unseen) {
					RevealUnseenBox(intPos[X] - size / 2, intPos[Y] - size / 2, size, size, team);
					affectedAny = true;
				}
			} else {
				if (!is_unseen) {
					RestoreUnseenBox(intPos[X] - size / 2, intPos[Y] - size / 2, size, size, team);
					affectedAny = true;
				}
			}

			// Check the strength of the terrain to see if we can penetrate further
			materialID = GetTerrMatter(intPos[X], intPos[Y]);
			// Get the material object
			foundMaterial = GetMaterialFromID(materialID);
			// Add the encountered material's strength to the tally
			totalStrength += foundMaterial->GetIntegrity();
			// See if we have hit the limits of our ray's strength
			if (totalStrength >= strengthLimit) {
				// Save the position of the end of the ray where blocked
				endPos.SetXY(intPos[X], intPos[Y]);
				break;
			}
			// Reset skip counter
			skipped = 0;
			if (m_pDebugLayer && m_DrawRayCastVisualizations) {
				m_pDebugLayer->SetPixel(intPos[X], intPos[Y], 13);
			}
		}
	}

	return affectedAny;
}

bool SceneMan::CastSeeRay(int team, const Vector& start, const Vector& ray, Vector& endPos, int strengthLimit, int skip) {
	return CastUnseenRay(team, start, ray, endPos, strengthLimit, skip, true);
}

bool SceneMan::CastUnseeRay(int team, const Vector& start, const Vector& ray, Vector& endPos, int strengthLimit, int skip) {
	return CastUnseenRay(team, start, ray, endPos, strengthLimit, skip, false);
}

bool SceneMan::CastMaterialRay(const Vector& start, const Vector& ray, unsigned char material, Vector& result, int skip, bool wrap) {

	int error, dom, sub, domSteps, skipped = skip;
	int intPos[2], delta[2], delta2[2], increment[2];
	bool foundPixel = false;

	intPos[X] = std::floor(start.m_X);
	intPos[Y] = std::floor(start.m_Y);
	delta[X] = std::floor(start.m_X + ray.m_X) - intPos[X];
	delta[Y] = std::floor(start.m_Y + ray.m_Y) - intPos[Y];

	if (delta[X] == 0 && delta[Y] == 0)
		return false;

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm preparation

	if (delta[X] < 0) {
		increment[X] = -1;
		delta[X] = -delta[X];
	} else
		increment[X] = 1;

	if (delta[Y] < 0) {
		increment[Y] = -1;
		delta[Y] = -delta[Y];
	} else
		increment[Y] = 1;

	// Scale by 2, for better accuracy of the error at the first pixel
	delta2[X] = delta[X] << 1;
	delta2[Y] = delta[Y] << 1;

	// If X is dominant, Y is submissive, and vice versa.
	if (delta[X] > delta[Y]) {
		dom = X;
		sub = Y;
	} else {
		dom = Y;
		sub = X;
	}

	error = delta2[sub] - delta[dom];

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm execution

	for (domSteps = 0; domSteps < delta[dom]; ++domSteps) {
		intPos[dom] += increment[dom];
		if (error >= 0) {
			intPos[sub] += increment[sub];
			error -= delta2[dom];
		}
		error += delta2[sub];

		// Only check pixel if we're not due to skip any, or if this is the last pixel
		if (++skipped > skip || domSteps + 1 == delta[dom]) {
			// Scene wrapping, if necessary
			if (wrap)
				g_SceneMan.WrapPosition(intPos[X], intPos[Y]);

			// See if we found the looked-for pixel of the correct material
			if (GetTerrMatter(intPos[X], intPos[Y]) == material) {
				// Save result and report success
				foundPixel = true;
				result.SetXY(intPos[X], intPos[Y]);
				// Save last ray pos
				s_LastRayHitPos.SetXY(intPos[X], intPos[Y]);
				break;
			}

			skipped = 0;

			if (m_pDebugLayer && m_DrawRayCastVisualizations) {
				m_pDebugLayer->SetPixel(intPos[X], intPos[Y], 13);
			}
		}
	}

	return foundPixel;
}

float SceneMan::CastMaterialRay(const Vector& start, const Vector& ray, unsigned char material, int skip) {
	Vector result;
	if (CastMaterialRay(start, ray, material, result, skip)) {
		// Calculate the length between the start and the found material pixel coords
		result -= start;
		return result.GetMagnitude();
	}

	// Signal that we didn't hit anything
	return -1;
}

bool SceneMan::CastNotMaterialRay(const Vector& start, const Vector& ray, unsigned char material, Vector& result, int skip, bool checkMOs) {
	int error, dom, sub, domSteps, skipped = skip;
	int intPos[2], delta[2], delta2[2], increment[2];
	bool foundPixel = false;

	intPos[X] = std::floor(start.m_X);
	intPos[Y] = std::floor(start.m_Y);
	delta[X] = std::floor(start.m_X + ray.m_X) - intPos[X];
	delta[Y] = std::floor(start.m_Y + ray.m_Y) - intPos[Y];

	if (delta[X] == 0 && delta[Y] == 0)
		return false;

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm preparation

	if (delta[X] < 0) {
		increment[X] = -1;
		delta[X] = -delta[X];
	} else
		increment[X] = 1;

	if (delta[Y] < 0) {
		increment[Y] = -1;
		delta[Y] = -delta[Y];
	} else
		increment[Y] = 1;

	// Scale by 2, for better accuracy of the error at the first pixel
	delta2[X] = delta[X] << 1;
	delta2[Y] = delta[Y] << 1;

	// If X is dominant, Y is submissive, and vice versa.
	if (delta[X] > delta[Y]) {
		dom = X;
		sub = Y;
	} else {
		dom = Y;
		sub = X;
	}

	error = delta2[sub] - delta[dom];

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm execution

	for (domSteps = 0; domSteps < delta[dom]; ++domSteps) {
		intPos[dom] += increment[dom];
		if (error >= 0) {
			intPos[sub] += increment[sub];
			error -= delta2[dom];
		}
		error += delta2[sub];

		// Only check pixel if we're not due to skip any, or if this is the last pixel
		if (++skipped > skip || domSteps + 1 == delta[dom]) {
			// Scene wrapping, if necessary
			g_SceneMan.WrapPosition(intPos[X], intPos[Y]);

			// See if we found the looked-for pixel of the correct material,
			// Or an MO is blocking the way
			if (GetTerrMatter(intPos[X], intPos[Y]) != material ||
			    (checkMOs && g_SceneMan.GetMOIDPixel(intPos[X], intPos[Y], Activity::NoTeam) != g_NoMOID)) {
				// Save result and report success
				foundPixel = true;
				result.SetXY(intPos[X], intPos[Y]);
				// Save last ray pos
				s_LastRayHitPos.SetXY(intPos[X], intPos[Y]);
				break;
			}

			skipped = 0;
			if (m_pDebugLayer && m_DrawRayCastVisualizations) {
				m_pDebugLayer->SetPixel(intPos[X], intPos[Y], 13);
			}
		}
	}

	return foundPixel;
}

float SceneMan::CastNotMaterialRay(const Vector& start, const Vector& ray, unsigned char material, int skip, bool checkMOs) {
	Vector result;
	if (CastNotMaterialRay(start, ray, material, result, skip, checkMOs)) {
		// Calculate the length between the start and the found material pixel coords
		result -= start;
		return result.GetMagnitude();
	}

	// Signal that we didn't hit anything
	return -1;
}

float SceneMan::CastStrengthSumRay(const Vector& start, const Vector& end, int skip, unsigned char ignoreMaterial) {
	Vector ray = g_SceneMan.ShortestDistance(start, end);
	float strengthSum = 0;

	int error, dom, sub, domSteps, skipped = skip;
	int intPos[2], delta[2], delta2[2], increment[2];
	unsigned char materialID;
	Material foundMaterial;

	intPos[X] = std::floor(start.m_X);
	intPos[Y] = std::floor(start.m_Y);
	delta[X] = std::floor(start.m_X + ray.m_X) - intPos[X];
	delta[Y] = std::floor(start.m_Y + ray.m_Y) - intPos[Y];

	if (delta[X] == 0 && delta[Y] == 0)
		return false;

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm preparation

	if (delta[X] < 0) {
		increment[X] = -1;
		delta[X] = -delta[X];
	} else
		increment[X] = 1;

	if (delta[Y] < 0) {
		increment[Y] = -1;
		delta[Y] = -delta[Y];
	} else
		increment[Y] = 1;

	// Scale by 2, for better accuracy of the error at the first pixel
	delta2[X] = delta[X] << 1;
	delta2[Y] = delta[Y] << 1;

	// If X is dominant, Y is submissive, and vice versa.
	if (delta[X] > delta[Y]) {
		dom = X;
		sub = Y;
	} else {
		dom = Y;
		sub = X;
	}

	error = delta2[sub] - delta[dom];

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm execution

	for (domSteps = 0; domSteps < delta[dom]; ++domSteps) {
		intPos[dom] += increment[dom];
		if (error >= 0) {
			intPos[sub] += increment[sub];
			error -= delta2[dom];
		}
		error += delta2[sub];

		// Only check pixel if we're not due to skip any, or if this is the last pixel
		if (++skipped > skip || domSteps + 1 == delta[dom]) {
			// Scene wrapping, if necessary
			g_SceneMan.WrapPosition(intPos[X], intPos[Y]);

			// Sum all strengths
			materialID = GetTerrMatter(intPos[X], intPos[Y]);
			if (materialID != g_MaterialAir && materialID != ignoreMaterial) {
				strengthSum += GetMaterialFromID(materialID)->GetIntegrity();
			}

			skipped = 0;

			if (m_pDebugLayer && m_DrawRayCastVisualizations) {
				m_pDebugLayer->SetPixel(intPos[X], intPos[Y], 13);
			}
		}
	}

	return strengthSum;
}

float SceneMan::CastMaxStrengthRay(const Vector& start, const Vector& end, int skip, unsigned char ignoreMaterial) {
	return CastMaxStrengthRayMaterial(start, end, skip, ignoreMaterial)->GetIntegrity();
}

const Material* SceneMan::CastMaxStrengthRayMaterial(const Vector& start, const Vector& end, int skip, unsigned char ignoreMaterial) {
	Vector ray = g_SceneMan.ShortestDistance(start, end);
	const Material* strongestMaterial = GetMaterialFromID(MaterialColorKeys::g_MaterialAir);

	int error, dom, sub, domSteps, skipped = skip;
	int intPos[2], delta[2], delta2[2], increment[2];

	intPos[X] = std::floor(start.m_X);
	intPos[Y] = std::floor(start.m_Y);
	delta[X] = std::floor(start.m_X + ray.m_X) - intPos[X];
	delta[Y] = std::floor(start.m_Y + ray.m_Y) - intPos[Y];

	if (delta[X] == 0 && delta[Y] == 0) {
		return strongestMaterial;
	}

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm preparation

	if (delta[X] < 0) {
		increment[X] = -1;
		delta[X] = -delta[X];
	} else
		increment[X] = 1;

	if (delta[Y] < 0) {
		increment[Y] = -1;
		delta[Y] = -delta[Y];
	} else
		increment[Y] = 1;

	// Scale by 2, for better accuracy of the error at the first pixel
	delta2[X] = delta[X] << 1;
	delta2[Y] = delta[Y] << 1;

	// If X is dominant, Y is submissive, and vice versa.
	if (delta[X] > delta[Y]) {
		dom = X;
		sub = Y;
	} else {
		dom = Y;
		sub = X;
	}

	error = delta2[sub] - delta[dom];

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm execution

	for (domSteps = 0; domSteps < delta[dom]; ++domSteps) {
		intPos[dom] += increment[dom];
		if (error >= 0) {
			intPos[sub] += increment[sub];
			error -= delta2[dom];
		}
		error += delta2[sub];

		// Only check pixel if we're not due to skip any, or if this is the last pixel
		if (++skipped > skip || domSteps + 1 == delta[dom]) {
			// Scene wrapping, if necessary
			g_SceneMan.WrapPosition(intPos[X], intPos[Y]);

			// Sum all strengths
			unsigned char materialID = GetTerrMatter(intPos[X], intPos[Y]);
			if (materialID != g_MaterialAir && materialID != ignoreMaterial) {
				const Material* foundMaterial = GetMaterialFromID(materialID);
				if (foundMaterial->GetIntegrity() > strongestMaterial->GetIntegrity()) {
					strongestMaterial = foundMaterial;
				}
			}

			skipped = 0;

			if (m_pDebugLayer && m_DrawRayCastVisualizations) {
				m_pDebugLayer->SetPixel(intPos[X], intPos[Y], 13);
			}
		}
	}

	return strongestMaterial;
}

bool SceneMan::CastStrengthRay(const Vector& start, const Vector& ray, float strength, Vector& result, int skip, unsigned char ignoreMaterial, bool wrap) {
	int error, dom, sub, domSteps, skipped = skip;
	int intPos[2], delta[2], delta2[2], increment[2];
	bool foundPixel = false;
	unsigned char materialID;
	Material const* foundMaterial;

	intPos[X] = std::floor(start.m_X);
	intPos[Y] = std::floor(start.m_Y);
	delta[X] = std::floor(start.m_X + ray.m_X) - intPos[X];
	delta[Y] = std::floor(start.m_Y + ray.m_Y) - intPos[Y];

	if (delta[X] == 0 && delta[Y] == 0)
		return false;

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm preparation

	if (delta[X] < 0) {
		increment[X] = -1;
		delta[X] = -delta[X];
	} else
		increment[X] = 1;

	if (delta[Y] < 0) {
		increment[Y] = -1;
		delta[Y] = -delta[Y];
	} else
		increment[Y] = 1;

	// Scale by 2, for better accuracy of the error at the first pixel
	delta2[X] = delta[X] << 1;
	delta2[Y] = delta[Y] << 1;

	// If X is dominant, Y is submissive, and vice versa.
	if (delta[X] > delta[Y]) {
		dom = X;
		sub = Y;
	} else {
		dom = Y;
		sub = X;
	}

	error = delta2[sub] - delta[dom];

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm execution

	for (domSteps = 0; domSteps < delta[dom]; ++domSteps) {
		intPos[dom] += increment[dom];
		if (error >= 0) {
			intPos[sub] += increment[sub];
			error -= delta2[dom];
		}
		error += delta2[sub];

		// Only check pixel if we're not due to skip any, or if this is the last pixel
		if (++skipped > skip || domSteps + 1 == delta[dom]) {
			// Scene wrapping, if necessary
			if (wrap)
				g_SceneMan.WrapPosition(intPos[X], intPos[Y]);

			materialID = GetTerrMatter(intPos[X], intPos[Y]);
			// Ignore the ignore material
			if (materialID != ignoreMaterial) {
				// Get the material object
				foundMaterial = GetMaterialFromID(materialID);

				// See if we found a pixel of equal or more strength than the threshold
				if (foundMaterial->GetIntegrity() >= strength) {
					// Save result and report success
					foundPixel = true;
					result.SetXY(intPos[X], intPos[Y]);
					// Save last ray pos
					s_LastRayHitPos.SetXY(intPos[X], intPos[Y]);
					break;
				}
			}
			skipped = 0;

			if (m_pDebugLayer && m_DrawRayCastVisualizations) {
				m_pDebugLayer->SetPixel(intPos[X], intPos[Y], 13);
			}
		}
	}

	// If no pixel of sufficient strength was found, set the result to the final tried position
	if (!foundPixel)
		result.SetXY(intPos[X], intPos[Y]);

	return foundPixel;
}

bool SceneMan::CastWeaknessRay(const Vector& start, const Vector& ray, float strength, Vector& result, int skip, bool wrap) {
	int error, dom, sub, domSteps, skipped = skip;
	int intPos[2], delta[2], delta2[2], increment[2];
	bool foundPixel = false;
	unsigned char materialID;
	Material const* foundMaterial;

	intPos[X] = std::floor(start.m_X);
	intPos[Y] = std::floor(start.m_Y);
	delta[X] = std::floor(start.m_X + ray.m_X) - intPos[X];
	delta[Y] = std::floor(start.m_Y + ray.m_Y) - intPos[Y];

	if (delta[X] == 0 && delta[Y] == 0)
		return false;

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm preparation

	if (delta[X] < 0) {
		increment[X] = -1;
		delta[X] = -delta[X];
	} else
		increment[X] = 1;

	if (delta[Y] < 0) {
		increment[Y] = -1;
		delta[Y] = -delta[Y];
	} else
		increment[Y] = 1;

	// Scale by 2, for better accuracy of the error at the first pixel
	delta2[X] = delta[X] << 1;
	delta2[Y] = delta[Y] << 1;

	// If X is dominant, Y is submissive, and vice versa.
	if (delta[X] > delta[Y]) {
		dom = X;
		sub = Y;
	} else {
		dom = Y;
		sub = X;
	}

	error = delta2[sub] - delta[dom];

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm execution

	for (domSteps = 0; domSteps < delta[dom]; ++domSteps) {
		intPos[dom] += increment[dom];
		if (error >= 0) {
			intPos[sub] += increment[sub];
			error -= delta2[dom];
		}
		error += delta2[sub];

		// Only check pixel if we're not due to skip any, or if this is the last pixel
		if (++skipped > skip || domSteps + 1 == delta[dom]) {
			// Scene wrapping, if necessary
			if (wrap)
				g_SceneMan.WrapPosition(intPos[X], intPos[Y]);

			materialID = GetTerrMatter(intPos[X], intPos[Y]);
			foundMaterial = GetMaterialFromID(materialID);

			// See if we found a pixel of equal or less strength than the threshold
			if (foundMaterial->GetIntegrity() <= strength) {
				// Save result and report success
				foundPixel = true;
				result.SetXY(intPos[X], intPos[Y]);
				// Save last ray pos
				s_LastRayHitPos.SetXY(intPos[X], intPos[Y]);
				break;
			}

			skipped = 0;

			if (m_pDebugLayer && m_DrawRayCastVisualizations) {
				m_pDebugLayer->SetPixel(intPos[X], intPos[Y], 13);
			}
		}
	}

	// If no pixel of sufficient strength was found, set the result to the final tried position
	if (!foundPixel)
		result.SetXY(intPos[X], intPos[Y]);

	return foundPixel;
}

MOID SceneMan::CastMORay(const Vector& start, const Vector& ray, const std::vector<MOID>& ignoreMOIDs, int ignoreTeam, unsigned char ignoreMaterial, bool ignoreAllTerrain, int skip) {
	int error, dom, sub, domSteps, skipped = skip;
	int intPos[2], delta[2], delta2[2], increment[2];
	MOID hitMOID = g_NoMOID;
	unsigned char hitTerrain = 0;

	intPos[X] = std::floor(start.m_X);
	intPos[Y] = std::floor(start.m_Y);
	delta[X] = std::floor(start.m_X + ray.m_X) - intPos[X];
	delta[Y] = std::floor(start.m_Y + ray.m_Y) - intPos[Y];

	if (delta[X] == 0 && delta[Y] == 0)
		return g_NoMOID;

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm preparation

	if (delta[X] < 0) {
		increment[X] = -1;
		delta[X] = -delta[X];
	} else
		increment[X] = 1;

	if (delta[Y] < 0) {
		increment[Y] = -1;
		delta[Y] = -delta[Y];
	} else
		increment[Y] = 1;

	// Scale by 2, for better accuracy of the error at the first pixel
	delta2[X] = delta[X] << 1;
	delta2[Y] = delta[Y] << 1;

	// If X is dominant, Y is submissive, and vice versa.
	if (delta[X] > delta[Y]) {
		dom = X;
		sub = Y;
	} else {
		dom = Y;
		sub = X;
	}

	error = delta2[sub] - delta[dom];

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm execution

	for (domSteps = 0; domSteps < delta[dom]; ++domSteps) {
		intPos[dom] += increment[dom];
		if (error >= 0) {
			intPos[sub] += increment[sub];
			error -= delta2[dom];
		}
		error += delta2[sub];

		// Only check pixel if we're not due to skip any, or if this is the last pixel
		if (++skipped > skip || domSteps + 1 == delta[dom]) {

			// Scene wrapping, if necessary
			g_SceneMan.WrapPosition(intPos[X], intPos[Y]);

			// Detect MOIDs
			hitMOID = GetMOIDPixel(intPos[X], intPos[Y], ignoreTeam);

			// Loop through ignored MOIDs to see if the one we found is ignored
			bool ignoredMOIDHit = false;
			for (auto ignoredMOID : ignoreMOIDs) {
				if (hitMOID == ignoredMOID || g_MovableMan.GetRootMOID(hitMOID) == ignoredMOID) {
					ignoredMOIDHit = true;
					break;
				}
			}
			
			if (hitMOID != g_NoMOID && !ignoredMOIDHit) {
				// Save last ray pos
				s_LastRayHitPos.SetXY(intPos[X], intPos[Y]);
				return hitMOID;
			}

			// Detect terrain hits
			if (!ignoreAllTerrain) {
				hitTerrain = g_SceneMan.GetTerrMatter(intPos[X], intPos[Y]);
				if (hitTerrain != g_MaterialAir && hitTerrain != ignoreMaterial) {
					// Save last ray pos
					s_LastRayHitPos.SetXY(intPos[X], intPos[Y]);
					return g_NoMOID;
				}
			}

			skipped = 0;

			if (m_pDebugLayer && m_DrawRayCastVisualizations) {
				m_pDebugLayer->SetPixel(intPos[X], intPos[Y], 13);
			}
		}
	}

	// Didn't hit anything but air
	return g_NoMOID;
}

bool SceneMan::CastFindMORay(const Vector& start, const Vector& ray, MOID targetMOID, Vector& resultPos, unsigned char ignoreMaterial, bool ignoreAllTerrain, int skip, bool findChildMOIDs) {
	int error, dom, sub, domSteps, skipped = skip;
	int intPos[2], delta[2], delta2[2], increment[2];
	MOID hitMOID = g_NoMOID;
	unsigned char hitTerrain = 0;

	intPos[X] = std::floor(start.m_X);
	intPos[Y] = std::floor(start.m_Y);
	delta[X] = std::floor(start.m_X + ray.m_X) - intPos[X];
	delta[Y] = std::floor(start.m_Y + ray.m_Y) - intPos[Y];

	if (delta[X] == 0 && delta[Y] == 0)
		return g_NoMOID;

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm preparation

	if (delta[X] < 0) {
		increment[X] = -1;
		delta[X] = -delta[X];
	} else
		increment[X] = 1;

	if (delta[Y] < 0) {
		increment[Y] = -1;
		delta[Y] = -delta[Y];
	} else
		increment[Y] = 1;

	// Scale by 2, for better accuracy of the error at the first pixel
	delta2[X] = delta[X] << 1;
	delta2[Y] = delta[Y] << 1;

	// If X is dominant, Y is submissive, and vice versa.
	if (delta[X] > delta[Y]) {
		dom = X;
		sub = Y;
	} else {
		dom = Y;
		sub = X;
	}

	error = delta2[sub] - delta[dom];

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm execution

	for (domSteps = 0; domSteps < delta[dom]; ++domSteps) {
		intPos[dom] += increment[dom];
		if (error >= 0) {
			intPos[sub] += increment[sub];
			error -= delta2[dom];
		}
		error += delta2[sub];

		// Only check pixel if we're not due to skip any, or if this is the last pixel
		if (++skipped > skip || domSteps + 1 == delta[dom]) {
			// Scene wrapping, if necessary
			g_SceneMan.WrapPosition(intPos[X], intPos[Y]);

			// Detect MOIDs
			hitMOID = GetMOIDPixel(intPos[X], intPos[Y], Activity::NoTeam);
			if (hitMOID == targetMOID || (findChildMOIDs && hitMOID == g_MovableMan.GetRootMOID(targetMOID))) {
				// Found target MOID, so save result and report success
				resultPos.SetXY(intPos[X], intPos[Y]);
				// Save last ray pos
				s_LastRayHitPos.SetXY(intPos[X], intPos[Y]);
				return true;
			}

			// Detect terrain hits
			if (!ignoreAllTerrain) {
				hitTerrain = g_SceneMan.GetTerrMatter(intPos[X], intPos[Y]);
				if (hitTerrain != g_MaterialAir && hitTerrain != ignoreMaterial) {
					// Save last ray pos
					s_LastRayHitPos.SetXY(intPos[X], intPos[Y]);
					return false;
				}
			}

			skipped = 0;

			if (m_pDebugLayer && m_DrawRayCastVisualizations) {
				m_pDebugLayer->SetPixel(intPos[X], intPos[Y], 13);
			}
		}
	}

	// Didn't hit the target
	return false;
}

const std::vector<MovableObject*>*  SceneMan::CastAllMOsRay(const Vector& start, const Vector& ray, const std::vector<MOID>& ignoreMOIDs, int ignoreTeam, unsigned char ignoreMaterial, bool ignoreAllTerrain, int skip) const {
	std::vector<MovableObject*>* vectorForLua = new std::vector<MovableObject*>();

	const SpatialPartitionGrid& partitionGrid = GetMOIDGrid();

	int error, dom, sub, domSteps, skipped = skip;
	int intPos[2], delta[2], delta2[2], increment[2];
	unsigned char hitTerrain = 0;

	intPos[X] = std::floor(start.m_X);
	intPos[Y] = std::floor(start.m_Y);
	delta[X] = std::floor(start.m_X + ray.m_X) - intPos[X];
	delta[Y] = std::floor(start.m_Y + ray.m_Y) - intPos[Y];

	if (delta[X] == 0 && delta[Y] == 0)
		return vectorForLua;

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm preparation

	if (delta[X] < 0) {
		increment[X] = -1;
		delta[X] = -delta[X];
	} else
		increment[X] = 1;

	if (delta[Y] < 0) {
		increment[Y] = -1;
		delta[Y] = -delta[Y];
	} else
		increment[Y] = 1;

	// Scale by 2, for better accuracy of the error at the first pixel
	delta2[X] = delta[X] << 1;
	delta2[Y] = delta[Y] << 1;

	// If X is dominant, Y is submissive, and vice versa.
	if (delta[X] > delta[Y]) {
		dom = X;
		sub = Y;
	} else {
		dom = Y;
		sub = X;
	}

	error = delta2[sub] - delta[dom];

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm execution

	for (domSteps = 0; domSteps < delta[dom]; ++domSteps) {
		intPos[dom] += increment[dom];
		if (error >= 0) {
			intPos[sub] += increment[sub];
			error -= delta2[dom];
		}
		error += delta2[sub];

		// Only check pixel if we're not due to skip any, or if this is the last pixel
		if (++skipped > skip || domSteps + 1 == delta[dom]) {

			// Scene wrapping, if necessary
			g_SceneMan.WrapPosition(intPos[X], intPos[Y]);
				
			// Detect MOs
			std::vector<MovableObject*> hitMOs;
			hitMOs = partitionGrid.GetMOsAtPosition(intPos[X], intPos[Y], ignoreTeam, false);

			// Loop through the gotten MOs and check if we're ignoring their IDs - if not, put them onto our return vector
			for (MovableObject* mo : hitMOs) {
				MOID moid = mo->GetID();
				for (auto ignoredMOID : ignoreMOIDs) {
					if (moid != ignoredMOID && g_MovableMan.GetRootMOID(moid) != ignoredMOID) {
						// Save last ray pos
						s_LastRayHitPos.SetXY(intPos[X], intPos[Y]);
						vectorForLua->push_back(mo);
					}
				}
			}

			// Detect terrain hits
			if (!ignoreAllTerrain) {
				hitTerrain = g_SceneMan.GetTerrMatter(intPos[X], intPos[Y]);
				if (hitTerrain != g_MaterialAir && hitTerrain != ignoreMaterial) {
					// Save last ray pos
					s_LastRayHitPos.SetXY(intPos[X], intPos[Y]);
					return vectorForLua;
				}
			}

			skipped = 0;

			if (m_pDebugLayer && m_DrawRayCastVisualizations) {
				m_pDebugLayer->SetPixel(intPos[X], intPos[Y], 13);
			}
		}
	}

	// Didn't hit anything but air
	return vectorForLua;
}

float SceneMan::CastObstacleRay(const Vector& start, const Vector& ray, Vector& obstaclePos, Vector& freePos, const std::vector<MOID>& ignoreMOIDs, int ignoreTeam, unsigned char ignoreMaterial, int skip) {
	int error, dom, sub, domSteps, skipped = skip;
	int intPos[2], delta[2], delta2[2], increment[2];
	bool hitObstacle = false;

	intPos[X] = std::floor(start.m_X);
	intPos[Y] = std::floor(start.m_Y);
	delta[X] = std::floor(start.m_X + ray.m_X) - intPos[X];
	delta[Y] = std::floor(start.m_Y + ray.m_Y) - intPos[Y];
	// The fraction of a pixel that we start from, to be added to the integer result positions for accuracy
	Vector startFraction(start.m_X - intPos[X], start.m_Y - intPos[Y]);

	if (delta[X] == 0 && delta[Y] == 0) {
		return -1.0f;
	}

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm preparation

	if (delta[X] < 0) {
		increment[X] = -1;
		delta[X] = -delta[X];
	} else {
		increment[X] = 1;
	}

	if (delta[Y] < 0) {
		increment[Y] = -1;
		delta[Y] = -delta[Y];
	} else {
		increment[Y] = 1;
	}

	// Scale by 2, for better accuracy of the error at the first pixel
	delta2[X] = delta[X] * 2;
	delta2[Y] = delta[Y] * 2;

	// If X is dominant, Y is submissive, and vice versa.
	if (delta[X] > delta[Y]) {
		dom = X;
		sub = Y;
	} else {
		dom = Y;
		sub = X;
	}

	error = delta2[sub] - delta[dom];

	/////////////////////////////////////////////////////
	// Bresenham's line drawing algorithm execution

	for (domSteps = 0; domSteps < delta[dom]; ++domSteps) {
		intPos[dom] += increment[dom];
		if (error >= 0) {
			intPos[sub] += increment[sub];
			error -= delta2[dom];
		}
		error += delta2[sub];

		// Only check pixel if we're not due to skip any, or if this is the last pixel
		if (++skipped > skip || domSteps + 1 == delta[dom]) {
			// Scene wrapping, if necessary
			g_SceneMan.WrapPosition(intPos[X], intPos[Y]);

			unsigned char checkMat = GetTerrMatter(intPos[X], intPos[Y]);
			MOID checkMOID = GetMOIDPixel(intPos[X], intPos[Y], ignoreTeam);

			// Loop through ignored MOIDs to see if the one we found is ignored
			bool ignoredMOIDHit = false;
			for (auto ignoredMOID : ignoreMOIDs) {
				if (checkMOID == ignoredMOID || g_MovableMan.GetRootMOID(checkMOID) == ignoredMOID) {
					ignoredMOIDHit = true;
					break;
				}
			}

			// See if we found the looked-for pixel of the correct material,
			// Or an MO is blocking the way
			if ((checkMat != g_MaterialAir && checkMat != ignoreMaterial) || (checkMOID != g_NoMOID && !ignoredMOIDHit)) {
				hitObstacle = true;
				obstaclePos.SetXY(intPos[X], intPos[Y]);
				// Save last ray pos
				s_LastRayHitPos.SetXY(intPos[X], intPos[Y]);
				break;
			} else {
				freePos.SetXY(intPos[X], intPos[Y]);
			}

			skipped = 0;

			if (m_pDebugLayer && m_DrawRayCastVisualizations) {
				m_pDebugLayer->SetPixel(intPos[X], intPos[Y], 13);
			}
		} else {
			freePos.SetXY(intPos[X], intPos[Y]);
		}
	}

	// Add the pixel fraction to the free position if there were any free pixels
	if (domSteps != 0) {
		freePos += startFraction;
	}

	if (hitObstacle) {
		// Add the pixel fraction to the obstacle position, to acoid losing precision
		obstaclePos += startFraction;
		if (domSteps == 0) {
			// If there was an obstacle on the start position, return 0 as the distance to obstacle
			return 0.0F;
		} else {
			// Calculate the length between the start and the found material pixel coords
			return g_SceneMan.ShortestDistance(obstaclePos, start).GetMagnitude();
		}
	}

	// Didn't hit anything but air
	return -1.0F;
}

const Vector& SceneMan::GetLastRayHitPos() {
	// The absolute end position of the last ray cast
	return s_LastRayHitPos;
}

float SceneMan::FindAltitude(const Vector& from, int max, int accuracy, bool fromSceneOrbitDirection) {
	// TODO: Also make this avoid doors
	Vector temp(from);
	ForceBounds(temp);

	Directions orbitDirection = Directions::Up;
	if (fromSceneOrbitDirection && m_pCurrentScene) {
		orbitDirection = m_pCurrentScene->GetTerrain()->GetOrbitDirection();
	}

	float yDir = max > 0 ? max : g_SceneMan.GetSceneHeight();
	yDir *= orbitDirection == Directions::Up ? 1.0 : -1.0f;
	Vector direction = Vector(0, yDir);

	float result = g_SceneMan.CastNotMaterialRay(temp, direction, g_MaterialAir, accuracy);
	// If we didn't find anything but air, then report max height
	if (result < 0) {
		result = max > 0 ? max : g_SceneMan.GetSceneHeight();
	}

	return orbitDirection == Directions::Up ? result : g_SceneMan.GetSceneHeight() - result;
}

bool SceneMan::OverAltitude(const Vector& point, int threshold, int accuracy) {
	Vector temp(point);
	ForceBounds(temp);
	return g_SceneMan.CastNotMaterialRay(temp, Vector(0, threshold), g_MaterialAir, accuracy) < 0;
}

bool SceneMan::IsPointInNoGravArea(const Vector& point) const {
	// Todo, instead of a nograv area maybe best to tag certain areas as NoGrav. As otherwise it's tricky to keep track of when things are removed
	if (m_pCurrentScene) {
		Scene::Area* noGravArea = m_pCurrentScene->GetArea("NoGravityArea");
		if (noGravArea && noGravArea->IsInside(point)) {
			return true;
		}
	}

	return false;
}

Vector SceneMan::MovePointToGround(const Vector& from, int heightAboveGround, int accuracy, int maxDistance) {
	if (IsPointInNoGravArea(from)) {
		return from;
	}

	Vector temp(from);
	ForceBounds(temp);

	float altitude = FindAltitude(temp, g_SceneMan.GetSceneHeight(), accuracy);

	// If there's no ground beneath us, do nothing
	if (altitude == g_SceneMan.GetSceneHeight() || (maxDistance != 0 && altitude > maxDistance)) {
		return temp;
	}

	Vector groundPoint(temp.m_X, temp.m_Y + (altitude - heightAboveGround));
	return groundPoint;
}

bool SceneMan::IsWithinBounds(const int pixelX, const int pixelY, const int margin) const {
	if (m_pCurrentScene)
		return m_pCurrentScene->GetTerrain()->IsWithinBounds(pixelX, pixelY, margin);

	return false;
}

bool SceneMan::ForceBounds(int& posX, int& posY) const {
	RTEAssert(m_pCurrentScene, "Trying to access scene before there is one!");
	return m_pCurrentScene->GetTerrain()->ForceBounds(posX, posY);
}

bool SceneMan::ForceBounds(Vector& pos) const {
	RTEAssert(m_pCurrentScene, "Trying to access scene before there is one!");

	int posX = std::floor(pos.m_X);
	int posY = std::floor(pos.m_Y);

	bool wrapped = m_pCurrentScene->GetTerrain()->ForceBounds(posX, posY);

	pos.m_X = posX + (pos.m_X - std::floor(pos.m_X));
	pos.m_Y = posY + (pos.m_Y - std::floor(pos.m_Y));

	return wrapped;
}

bool SceneMan::WrapPosition(int& posX, int& posY) const {
	RTEAssert(m_pCurrentScene, "Trying to access scene before there is one!");
	return m_pCurrentScene->GetTerrain()->WrapPosition(posX, posY);
}

bool SceneMan::WrapPosition(Vector& pos) const {
	RTEAssert(m_pCurrentScene, "Trying to access scene before there is one!");

	int posX = std::floor(pos.m_X);
	int posY = std::floor(pos.m_Y);

	bool wrapped = m_pCurrentScene->GetTerrain()->WrapPosition(posX, posY);

	pos.m_X = posX + (pos.m_X - std::floor(pos.m_X));
	pos.m_Y = posY + (pos.m_Y - std::floor(pos.m_Y));

	return wrapped;
}

Vector SceneMan::SnapPosition(const Vector& pos, bool snap) const {
	Vector snappedPos = pos;

	if (snap) {
		snappedPos.m_X = std::floor((pos.m_X / SCENESNAPSIZE) + 0.5) * SCENESNAPSIZE;
		snappedPos.m_Y = std::floor((pos.m_Y / SCENESNAPSIZE) + 0.5) * SCENESNAPSIZE;
	}

	return snappedPos;
}

Vector SceneMan::ShortestDistance(Vector pos1, Vector pos2, bool checkBounds) const {
	if (!m_pCurrentScene)
		return Vector();

	if (checkBounds) {
		WrapPosition(pos1);
		WrapPosition(pos2);
	}

	Vector distance = pos2 - pos1;
	float sceneWidth = m_pCurrentScene->GetWidth();
	float sceneHeight = m_pCurrentScene->GetHeight();

	if (m_pCurrentScene->GetTerrain()->WrapsX()) {
		if (distance.m_X > 0) {
			if (distance.m_X > (sceneWidth / 2))
				distance.m_X -= sceneWidth;
		} else {
			if (abs(distance.m_X) > (sceneWidth / 2))
				distance.m_X += sceneWidth;
		}
	}

	if (m_pCurrentScene->GetTerrain()->WrapsY()) {
		if (distance.m_Y > 0) {
			if (distance.m_Y > (sceneHeight / 2))
				distance.m_Y -= sceneHeight;
		} else {
			if (abs(distance.m_Y) > (sceneHeight / 2))
				distance.m_Y += sceneHeight;
		}
	}

	return distance;
}

float SceneMan::ShortestDistanceX(float val1, float val2, bool checkBounds, int direction) const {
	if (!m_pCurrentScene)
		return 0;

	if (checkBounds) {
		int x1 = val1;
		int x2 = val2;
		int crap = 0;
		WrapPosition(x1, crap);
		WrapPosition(x2, crap);
		val1 = x1;
		val2 = x2;
	}

	float distance = val2 - val1;
	float sceneWidth = m_pCurrentScene->GetWidth();

	if (m_pCurrentScene->GetTerrain()->WrapsX()) {
		if (distance > 0) {
			if (distance > (sceneWidth / 2))
				distance -= sceneWidth;
		} else {
			if (abs(distance) > (sceneWidth / 2))
				distance += sceneWidth;
		}

		// Apply direction constraint if wrapped
		if (direction > 0 && distance < 0)
			distance += sceneWidth;
		else if (direction < 0 && distance > 0)
			distance -= sceneWidth;
	}

	return distance;
}

float SceneMan::ShortestDistanceY(float val1, float val2, bool checkBounds, int direction) const {
	if (!m_pCurrentScene)
		return 0;

	if (checkBounds) {
		int y1 = val1;
		int y2 = val2;
		int crap = 0;
		WrapPosition(crap, y1);
		WrapPosition(crap, y2);
		val1 = y1;
		val2 = y2;
	}

	float distance = val2 - val1;
	float sceneHeight = m_pCurrentScene->GetHeight();

	if (m_pCurrentScene->GetTerrain()->WrapsY()) {
		if (distance > 0) {
			if (distance > (sceneHeight / 2))
				distance -= sceneHeight;
		} else {
			if (abs(distance) > (sceneHeight / 2))
				distance += sceneHeight;
		}

		// Apply direction constraint if wrapped
		if (direction > 0 && distance < 0)
			distance += sceneHeight;
		else if (direction < 0 && distance > 0)
			distance -= sceneHeight;
	}

	return distance;
}

bool SceneMan::ObscuredPoint(int x, int y, int team) {
	bool obscured = m_pCurrentScene->GetTerrain()->GetPixel(x, y) != g_MaterialAir || GetMOIDPixel(x, y, Activity::NoTeam) != g_NoMOID;

	if (team != Activity::NoTeam)
		obscured = obscured || IsUnseen(x, y, team);

	return obscured;
}

int SceneMan::WrapRect(const IntRect& wrapRect, std::list<IntRect>& outputList) {
	// Always add at least one copy of the unwrapped rect
	int addedTimes = 1;
	outputList.push_back(wrapRect);

	// Only bother with wrap checking if the scene actually wraps around in X
	if (SceneWrapsX()) {
		int sceneWidth = GetSceneWidth();

		if (wrapRect.m_Left < 0) {
			outputList.push_back(wrapRect);
			outputList.back().m_Left += sceneWidth;
			outputList.back().m_Right += sceneWidth;
			addedTimes++;
		}
		if (wrapRect.m_Right >= sceneWidth) {
			outputList.push_back(wrapRect);
			outputList.back().m_Left -= sceneWidth;
			outputList.back().m_Right -= sceneWidth;
			addedTimes++;
		}
	}

	// Only bother with wrap checking if the scene actually wraps around in Y
	if (SceneWrapsY()) {
		int sceneHeight = GetSceneHeight();

		if (wrapRect.m_Top < 0) {
			outputList.push_back(wrapRect);
			outputList.back().m_Top += sceneHeight;
			outputList.back().m_Bottom += sceneHeight;
			addedTimes++;
		}
		if (wrapRect.m_Bottom >= sceneHeight) {
			outputList.push_back(wrapRect);
			outputList.back().m_Top -= sceneHeight;
			outputList.back().m_Bottom -= sceneHeight;
			addedTimes++;
		}
	}

	return addedTimes;
}

int SceneMan::WrapBox(const Box& wrapBox, std::list<Box>& outputList) {
	// Unflip the input box, or checking will be tedious
	Box flipBox(wrapBox);
	flipBox.Unflip();

	// Always add at least one copy of the unwrapped rect
	int addedTimes = 1;
	outputList.push_back(flipBox);

	// Only bother with wrap checking if the scene actually wraps around in X
	if (SceneWrapsX()) {
		int sceneWidth = GetSceneWidth();

		if (flipBox.m_Corner.m_X < 0) {
			outputList.push_back(flipBox);
			outputList.back().m_Corner.m_X += sceneWidth;
			addedTimes++;
		}
		if (flipBox.m_Corner.m_X + flipBox.m_Width >= sceneWidth) {
			outputList.push_back(flipBox);
			outputList.back().m_Corner.m_X -= sceneWidth;
			addedTimes++;
		}
	}

	// Only bother with wrap checking if the scene actually wraps around in Y
	if (SceneWrapsY()) {
		int sceneHeight = GetSceneHeight();

		if (flipBox.m_Corner.m_Y < 0) {
			outputList.push_back(flipBox);
			outputList.back().m_Corner.m_Y += sceneHeight;
			addedTimes++;
		}
		if (flipBox.m_Corner.m_Y + flipBox.m_Height >= sceneHeight) {
			outputList.push_back(flipBox);
			outputList.back().m_Corner.m_Y -= sceneHeight;
			addedTimes++;
		}
	}

	return addedTimes;
}

bool SceneMan::AddSceneObject(SceneObject* sceneObject) {
	bool result = false;
	if (sceneObject) {
		if (MovableObject* sceneObjectAsMovableObject = dynamic_cast<MovableObject*>(sceneObject)) {
			return g_MovableMan.AddMO(sceneObjectAsMovableObject);
		} else if (TerrainObject* sceneObjectAsTerrainObject = dynamic_cast<TerrainObject*>(sceneObject)) {
			result = m_pCurrentScene && sceneObjectAsTerrainObject->PlaceOnTerrain(m_pCurrentScene->GetTerrain());
			if (result) {
				Box airBox(sceneObjectAsTerrainObject->GetPos() + sceneObjectAsTerrainObject->GetBitmapOffset(), static_cast<float>(sceneObjectAsTerrainObject->GetBitmapWidth()), static_cast<float>(sceneObjectAsTerrainObject->GetBitmapHeight()));
				m_pCurrentScene->GetTerrain()->CleanAirBox(airBox, GetScene()->WrapsX(), GetScene()->WrapsY());
			}
		}
	}
	delete sceneObject;
	return result;
}

void SceneMan::Update(int screenId) {
	ZoneScoped;

	if (!m_pCurrentScene) {
		return;
	}

	m_LastUpdatedScreen = screenId;

	const Vector offset = g_CameraMan.GetRenderOffset(screenId);
	m_pMOColorLayer->SetOffset(offset);
	if (m_pDebugLayer) {
		m_pDebugLayer->SetOffset(offset);
	}

	SLTerrain* terrain = m_pCurrentScene->GetTerrain();
	terrain->SetOffset(offset);
	terrain->Update();

	// Background layers may scroll in fractions of the real offset and need special care to avoid jumping after having traversed wrapped edges, so they need the total offset without taking wrapping into account.
	const Vector& unwrappedOffset = g_CameraMan.GetUnwrappedOffset(screenId);
	for (SLBackground* backgroundLayer: m_pCurrentScene->GetBackLayers()) {
		backgroundLayer->SetOffset(unwrappedOffset);
		backgroundLayer->Update();
	}

	// Update the unseen obstruction layer for this team's screen view, if there is one.
	const int teamId = g_CameraMan.GetScreenTeam(screenId);
	if (SceneLayer* unseenLayer = (teamId != Activity::NoTeam) ? m_pCurrentScene->GetUnseenLayer(teamId) : nullptr) {
		unseenLayer->SetOffset(offset);
	}

	if (m_CleanTimer.GetElapsedSimTimeMS() > CLEANAIRINTERVAL) {
		TraceTerrainEvent("clean", screenId, 0);
		terrain->CleanAir();
		m_CleanTimer.Reset();
	}
}

void SceneMan::Draw(BITMAP* targetBitmap, BITMAP* targetGUIBitmap, const Vector& targetPos, bool skipBackgroundLayers, bool skipTerrain) {
	ZoneScoped;

	if (!m_pCurrentScene) {
		return;
	}

	SLTerrain* terrain = m_pCurrentScene->GetTerrain();

	// Set up the target box to draw to on the target bitmap, if it is larger than the scene in either dimension.
	Box targetBox(Vector(), static_cast<float>(targetBitmap->w), static_cast<float>(targetBitmap->h));
	Box targetDimensions(Vector(), targetBitmap->w, targetBitmap->h);

	if (!terrain->WrapsX() && targetBitmap->w > GetSceneWidth()) {
		targetBox.SetCorner(Vector(static_cast<float>((targetBitmap->w - GetSceneWidth())) / 2, targetBox.GetCorner().GetY()));
		targetBox.SetWidth(static_cast<float>(GetSceneWidth()));
	}
	if (!terrain->WrapsY() && targetBitmap->h > GetSceneHeight()) {
		targetBox.SetCorner(Vector(targetBox.GetCorner().GetX(), static_cast<float>((targetBitmap->h - GetSceneHeight())) / 2));
		targetBox.SetHeight(static_cast<float>(GetSceneHeight()));
	}

	switch (m_LayerDrawMode) {
		case LayerDrawMode::g_LayerTerrainMatter:
			terrain->SetLayerToDraw(SLTerrain::LayerType::MaterialLayer);
			terrain->Draw(targetDimensions, targetBox);
			break;
		default:
			if (!skipBackgroundLayers) {
				for (std::list<SLBackground*>::reverse_iterator backgroundLayer = m_pCurrentScene->GetBackLayers().rbegin(); backgroundLayer != m_pCurrentScene->GetBackLayers().rend(); ++backgroundLayer) {
					(*backgroundLayer)->Draw(targetDimensions, targetBox);
				}
			}
			if (!skipTerrain) {
				terrain->SetLayerToDraw(SLTerrain::LayerType::BackgroundLayer);
				terrain->Draw(targetDimensions, targetBox);
			}

			// TODO- it would really be much nicer to draw direct-to-screen, with no intermediate MO layer
			// but this is awkward with draw order due to how the GPU interacts
			if (m_LastUpdatedScreen == 0) {
				g_SceneMan.ClearMOColorLayer();
				g_MovableMan.Draw(g_SceneMan.GetMOColorBitmap());
			}

			m_pMOColorLayer->Draw(targetDimensions, targetBox);

			if (!skipTerrain) {
				terrain->SetLayerToDraw(SLTerrain::LayerType::ForegroundLayer);
				terrain->Draw(targetDimensions, targetBox);
			}
			
			int teamId = g_CameraMan.GetScreenTeam(m_LastUpdatedScreen);
			if (SceneLayer* unseenLayer = (teamId != Activity::NoTeam) ? m_pCurrentScene->GetUnseenLayer(teamId) : nullptr) {
				unseenLayer->Draw(targetDimensions, targetBox);
			}

			bool shouldDrawHUD = !g_FrameMan.IsHudDisabled(m_LastUpdatedScreen);
			if (shouldDrawHUD) {
				g_MovableMan.DrawHUD(targetGUIBitmap, targetPos, m_LastUpdatedScreen);
			}

			if (shouldDrawHUD) {
				g_ActivityMan.GetActivity()->DrawGUI(targetGUIBitmap, targetPos, m_LastUpdatedScreen);
			}

			static bool s_drawNoGravBoxes = false;
			if (s_drawNoGravBoxes) {
				if (Scene::Area* noGravArea = m_pCurrentScene->GetArea("NoGravityArea")) {
					const std::vector<Box*>& boxList = noGravArea->GetBoxes();
					g_FrameMan.SetTransTableFromPreset(TransparencyPreset::MoreTrans);
					drawing_mode(DRAW_MODE_TRANS, 0, 0, 0);

					std::list<Box> wrappedBoxes;
					for (Box* box: boxList) {
						wrappedBoxes.clear();
						g_SceneMan.WrapBox(*box, wrappedBoxes);

						for (std::list<Box>::iterator wItr = wrappedBoxes.begin(); wItr != wrappedBoxes.end(); ++wItr) {
							Vector adjCorner = (*wItr).GetCorner() - targetPos;
							rectfill(targetBitmap, adjCorner.m_X, adjCorner.m_Y, adjCorner.m_X + (*wItr).GetWidth(), adjCorner.m_Y + (*wItr).GetHeight(), g_RedColor);
						}
					}
				}
			}

			static int s_drawPathfinderDebugForTeam = -2;
			if (s_drawPathfinderDebugForTeam > -2) {
				m_pCurrentScene->GetPathFinder(static_cast<Activity::Teams>(s_drawPathfinderDebugForTeam)).DebugRender(targetBitmap, targetPos);
			}

			if (m_pDebugLayer) {
				m_pDebugLayer->Draw(targetDimensions, targetBox);
			}

			break;
	}
}

void SceneMan::ClearMOColorLayer() {
	m_pMOColorLayer->ClearBitmap(g_MaskColor);
	if (m_pDebugLayer) {
		m_pDebugLayer->ClearBitmap(g_MaskColor);
	}
}

void SceneMan::ClearSeenPixels() {
	if (!m_pCurrentScene)
		return;

	for (int team = Activity::TeamOne; team < Activity::MaxTeamCount; ++team)
		m_pCurrentScene->ClearSeenPixels(team);
}

void SceneMan::ClearCurrentScene() {
	m_pCurrentScene = nullptr;
}

BITMAP* SceneMan::GetIntermediateBitmapForSettlingIntoTerrain(int moDiameter) const {
	int bitmapSizeNeeded = static_cast<int>(std::ceil(static_cast<float>(moDiameter) / 16.0F)) * 16;
	for (const auto& [bitmapSize, bitmapPtr]: m_IntermediateSettlingBitmaps) {
		if (std::min(bitmapSize, bitmapSizeNeeded) >= bitmapSizeNeeded) {
			return bitmapPtr;
		}
	}
	return m_IntermediateSettlingBitmaps.back().second;
}

std::string SceneMan::SaveCheckpoint() const {
    CheckpointWriter writer("SceneMan2");
    VisitCheckpoint(writer, *this);
    writer(SaveMaterialCatalog());
    return writer.Text();
}

bool SceneMan::LoadCheckpoint(std::string_view text, bool validateOnly) {
    try {
        const bool legacy = text.starts_with("9 SceneMan1 ");
        CheckpointReader reader(text, legacy ? "SceneMan1" : "SceneMan2", validateOnly);
        VisitCheckpoint(reader, *this);
        if (!legacy) {
            std::string materials; reader.Value(materials);
            if (!LoadMaterialCatalog(materials, true)) return false;
            reader.OnCommit([this, materials] { if (!LoadMaterialCatalog(materials)) throw std::runtime_error("could not restore material catalog"); });
        }
        reader.Finish();
        return true;
    } catch (const std::exception&) { return false; }
}

bool SceneMan::PrepareCheckpointMaterials(std::string_view text, bool validateOnly) {
    try {
        const bool legacy = text.starts_with("9 SceneMan1 ");
        CheckpointReader reader(text, legacy ? "SceneMan1" : "SceneMan2", true);
        VisitCheckpoint(reader, *this);
        std::string materials;
        if (!legacy) reader.Value(materials);
        reader.Finish();
        return legacy || LoadMaterialCatalog(materials, validateOnly);
    } catch (const std::exception&) { return false; }
}

SceneMan::SceneSetAside::~SceneSetAside() {
	delete scene; delete color; delete debug; delete revealSound;
}

void SceneMan::SetAsideScene(SceneSetAside& state) {
	state.scene = std::exchange(m_pCurrentScene, nullptr);
	state.color = std::exchange(m_pMOColorLayer, nullptr);
	state.debug = std::exchange(m_pDebugLayer, nullptr);
	// The fixed-size orphan search buffer belongs to the manager and is cleared before each search.
	state.revealSound = std::exchange(m_pUnseenRevealSound, nullptr);
	state.toLoad = m_pSceneToLoad;
	state.placeObjects = m_PlaceObjects;
	state.placeUnits = m_PlaceUnits;
}

void SceneMan::ReinstateScene(SceneSetAside& state) {
	std::swap(m_pCurrentScene, state.scene);
	std::swap(m_pMOColorLayer, state.color);
	std::swap(m_pDebugLayer, state.debug);
	std::swap(m_pUnseenRevealSound, state.revealSound);
	m_pSceneToLoad = state.toLoad;
	m_PlaceObjects = state.placeObjects;
	m_PlaceUnits = state.placeUnits;
}

namespace {
    struct CheckpointMaterialReference {
        int kind = 0; // null, palette, copied material, PresetMan material
        size_t index = 0;
        bool Load(std::string_view text) {
            try {
                CheckpointReader reader(text, "MaterialReference1");
                reader.Value(kind); reader.Value(index); reader.Finish();
                return kind >= 0 && kind <= 3 && (kind != 0 || index == 0) && (kind != 1 || index < c_PaletteEntriesNumber);
            } catch (const std::exception&) { return false; }
        }
        std::string Save() const { CheckpointWriter writer("MaterialReference1"); writer(kind, index); return writer.Text(); }
    };

    std::vector<Material*> CheckpointMaterialPresets() {
        std::list<Entity*> entities;
        g_PresetMan.GetAllOfType(entities, "Material");
        std::vector<Material*> materials;
        materials.reserve(entities.size());
        for (Entity* entity: entities) {
            auto* material = dynamic_cast<Material*>(entity);
            if (!material) throw std::runtime_error("non-material in Material preset collection");
            materials.push_back(material);
        }
        return materials;
    }

    struct CheckpointMaterialCatalog {
        int count = 0;
        std::map<std::string, unsigned char> names;
        std::array<std::string, c_PaletteEntriesNumber> palette;
        std::vector<std::string> copies, presets;
        template <class Archive> void Fields(Archive& archive) { archive(count, names, palette, copies, presets); }
        std::string Save() { CheckpointWriter writer("MaterialCatalog1"); Fields(writer); return writer.Text(); }
        bool Load(std::string_view text) {
            try {
                CheckpointReader reader(text, "MaterialCatalog1"); Fields(reader); reader.Finish();
                if (count < 0 || count > c_PaletteEntriesNumber || std::count_if(palette.begin(), palette.end(), [](const std::string& value) { return !value.empty(); }) != count) return false;
                Material validator;
                for (const std::string& value: palette) if (!value.empty() && !validator.LoadCheckpoint(value, true)) return false;
                for (const auto& values: {&copies, &presets}) for (const std::string& value: *values) if (!validator.LoadCheckpoint(value, true)) return false;
                for (const auto& [name, index]: names) if (palette[index].empty()) return false;
                return true;
            } catch (const std::exception&) { return false; }
        }
    };
}

std::string SceneMan::SaveMaterialReference(const Material* material) const {
    if (!material) return CheckpointMaterialReference{}.Save();
    for (size_t index = 0; index < m_apMatPalette.size(); ++index) if (m_apMatPalette[index] == material) return CheckpointMaterialReference{1, index}.Save();
    if (const auto copy = m_MaterialCopyIndices.find(material); copy != m_MaterialCopyIndices.end()) return CheckpointMaterialReference{2, copy->second}.Save();
    const auto presets = CheckpointMaterialPresets();
    const auto preset = std::find(presets.begin(), presets.end(), material);
    if (preset != presets.end()) return CheckpointMaterialReference{3, static_cast<size_t>(preset - presets.begin())}.Save();
    throw std::runtime_error("material has no checkpoint owner");
}

bool SceneMan::ValidateMaterialReference(std::string_view text) {
    CheckpointMaterialReference reference;
    return reference.Load(text);
}

const Material* SceneMan::ResolveMaterialReference(std::string_view text, bool allowMissing) const {
    CheckpointMaterialReference reference;
    if (!reference.Load(text)) throw std::runtime_error("invalid material checkpoint reference");
    if (reference.kind == 0) return nullptr;
    if (reference.kind == 1) {
        if (!m_apMatPalette[reference.index] && !allowMissing) throw std::runtime_error("material palette checkpoint target is missing");
        return m_apMatPalette[reference.index];
    }
    if (reference.kind == 2) {
        if (reference.index >= m_MaterialCopiesVector.size()) { if (allowMissing) return nullptr; throw std::runtime_error("copied material checkpoint target is missing"); }
        return m_MaterialCopiesVector[reference.index];
    }
    const auto presets = CheckpointMaterialPresets();
    if (reference.index >= presets.size()) { if (allowMissing) return nullptr; throw std::runtime_error("material preset checkpoint target is missing"); }
    return presets[reference.index];
}

std::string SceneMan::SaveMaterialCatalog() const {
    CheckpointMaterialCatalog state;
    state.count = m_MaterialCount;
    state.names = m_MatNameMap;
    for (size_t index = 0; index < m_apMatPalette.size(); ++index) if (m_apMatPalette[index]) state.palette[index] = m_apMatPalette[index]->SaveCheckpoint();
    for (const Material* material: m_MaterialCopiesVector) state.copies.push_back(material->SaveCheckpoint());
    for (const Material* material: CheckpointMaterialPresets()) state.presets.push_back(material->SaveCheckpoint());
    return state.Save();
}

bool SceneMan::LoadMaterialCatalog(std::string_view text, bool validateOnly) {
    CheckpointMaterialCatalog state;
    if (!state.Load(text)) return false;
    const auto presets = CheckpointMaterialPresets();
    if (presets.size() != state.presets.size()) return false;
    if (validateOnly) return true;
    try {
        // Resolve all borrowed assets and allocate all new owners before changing a
        // live Material. Existing owners are updated in place to retain native aliases.
        const auto makeValue = [](const std::string& saved) {
            std::unique_ptr<Material> value;
            if (!saved.empty()) {
                value = std::make_unique<Material>();
                if (!value->LoadCheckpoint(saved)) throw std::runtime_error("could not prepare material checkpoint");
            }
            return value;
        };
        std::array<std::unique_ptr<Material>, c_PaletteEntriesNumber> paletteValues;
        std::vector<std::unique_ptr<Material>> copyValues, presetValues;
        for (size_t index = 0; index < paletteValues.size(); ++index) paletteValues[index] = makeValue(state.palette[index]);
        for (const auto& saved: state.copies) copyValues.push_back(makeValue(saved));
        for (const auto& saved: state.presets) presetValues.push_back(makeValue(saved));

        auto retiredCopies = m_RetiredMaterialCopies;
        auto retiredPalette = m_RetiredPaletteMaterials;
        auto copyIndices = m_MaterialCopyIndices;
        std::vector<Material*> copies = m_MaterialCopiesVector;
        auto palette = m_apMatPalette;
        for (size_t index = state.copies.size(); index < copies.size(); ++index) retiredCopies[index].push_back(copies[index]);
        copies.resize(state.copies.size());
        for (size_t index = 0; index < copies.size(); ++index) {
            if (!copies[index]) {
                auto& retired = retiredCopies[index];
                copies[index] = retired.empty() ? copyValues[index].get() : retired.back();
                if (!retired.empty()) retired.pop_back();
                copyIndices[copies[index]] = index;
            }
        }
        for (size_t index = 0; index < palette.size(); ++index) {
            if (!paletteValues[index] && palette[index]) {
                retiredPalette[index].push_back(palette[index]); palette[index] = nullptr;
            } else if (paletteValues[index] && !palette[index]) {
                palette[index] = retiredPalette[index].empty() ? paletteValues[index].get() : retiredPalette[index].back();
                if (!retiredPalette[index].empty()) retiredPalette[index].pop_back();
            }
        }

        const auto apply = [](Material& destination, Material& value) noexcept {
            if (&destination != &value) destination.SwapCheckpoint(value);
        };
        for (size_t index = 0; index < palette.size(); ++index) if (palette[index]) apply(*palette[index], *paletteValues[index]);
        for (size_t index = 0; index < copies.size(); ++index) apply(*copies[index], *copyValues[index]);
        for (size_t index = 0; index < presets.size(); ++index) apply(*presets[index], *presetValues[index]);
        for (size_t index = 0; index < palette.size(); ++index) if (palette[index] == paletteValues[index].get()) paletteValues[index].release();
        for (size_t index = 0; index < copies.size(); ++index) if (copies[index] == copyValues[index].get()) copyValues[index].release();
        m_apMatPalette.swap(palette);
        m_MaterialCopiesVector.swap(copies);
        m_MaterialCopyIndices.swap(copyIndices);
        m_RetiredMaterialCopies.swap(retiredCopies);
        m_RetiredPaletteMaterials.swap(retiredPalette);
        m_MatNameMap.swap(state.names);
        m_MaterialCount = state.count;
        return true;
    } catch (const std::exception&) { return false; }
}

bool SceneMan::RunMaterialCheckpointSelfTest() {
    const std::string original = SaveMaterialCatalog();
    bool passed = true;
    struct PenetrationCase { const char* name; float integrity, squaredImpulse, expected; };
    for (const auto& test: std::array<PenetrationCase, 6>{{
        {"zero_impulse_zero_strength", 0.0F, 0.0F, 0.0F},
        {"moving_particle_zero_strength", 0.0F, 16.0F, -0.0F},
        {"partial_resistance", 2.0F, 16.0F, -0.5F},
        {"penetration_threshold", 4.0F, 16.0F, -1.0F},
        {"fractional_resistance", 1.25F, 64.0F, -0.15625F},
        {"small_impulse", 0.125F, 0.0625F, -0.5F}
    }}) {
        const float actual = PenetrationRetardation(test.integrity, test.squaredImpulse);
        const bool result = std::bit_cast<uint32_t>(actual) == std::bit_cast<uint32_t>(test.expected);
        passed = result && passed;
        std::cout << "[penetration-selftest] " << (result ? "PASS " : "FAIL ") << test.name << std::endl;
    }
    try {
        Material* source = const_cast<Material*>(GetMaterialFromID(g_MaterialAir));
        if (!source) throw std::runtime_error("no source Material");
        Material* first = AddMaterialCopy(source);
        Material* second = AddMaterialCopy(source);
        const std::string firstReference = SaveMaterialReference(first), secondReference = SaveMaterialReference(second);
        if (firstReference == secondReference || ResolveMaterialReference(firstReference) != first || ResolveMaterialReference(secondReference) != second) throw std::runtime_error("distinct copied Materials lost their identities");
        const std::string captured = SaveMaterialCatalog();
        if (LoadMaterialCatalog(captured + "trailing") || SaveMaterialCatalog() != captured) throw std::runtime_error("failed material load changed the catalog");
        if (!LoadMaterialCatalog(original) || !LoadMaterialCatalog(captured) || SaveMaterialCatalog() != captured || ResolveMaterialReference(firstReference) != first || ResolveMaterialReference(secondReference) != second) throw std::runtime_error("material rollback changed values or aliases");
        Atom before(Vector(), first, nullptr), after;
        if (!after.LoadCheckpoint(before.SaveCheckpoint())) throw std::runtime_error("copied material Atom checkpoint was rejected");
        after.ResolveCheckpointLinks();
        if (after.GetMaterial() != first) throw std::runtime_error("Atom material resolved to a palette entry");
    } catch (const std::exception& error) {
        passed = false;
        std::cout << "[material-checkpoint] failure: " << error.what() << std::endl;
    }
    passed = LoadMaterialCatalog(original) && passed;
    std::cout << "[material-checkpoint] native values, distinct copies, alias restoration, rejected-load rollback: " << (passed ? "PASS" : "FAIL") << std::endl;
    return passed;
}
