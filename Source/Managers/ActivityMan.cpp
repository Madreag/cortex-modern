#include "ActivityMan.h"
#include "GUIInput.h"
#include "GUISound.h"
#include "CheckpointArchive.h"
#include "LuaMan.h"
#include "Base64/base64.h"

#include <filesystem>
#include "Activity.h"

#include "CameraMan.h"
#include "ConsoleMan.h"
#include "PresetMan.h"
#include "UInputMan.h"
#include "AudioMan.h"
#include "SoundSimulation.h"
#include "WindowMan.h"
#include "FrameMan.h"
#include "PerformanceMan.h"
#include "PostProcessMan.h"
#include "PrimitiveMan.h"
#include "MetaMan.h"
#include "ThreadMan.h"
#include "System.h"
#include "SaveGameArchive.h"

#include "GAScripted.h"
#include "GlobalScript.h"
#include "ACraft.h"
#include "SLTerrain.h"

#include "EditorActivity.h"
#include "SceneEditor.h"
#include "AreaEditor.h"
#include "GibEditor.h"
#include "ActorEditor.h"
#include "AssemblyEditor.h"

#include "MusicMan.h"

#ifdef SYSTEM_MINIZIP
#include <minizip/zip.h>
#include <minizip/unzip.h>
#else
#include "zip.h"
#include "unzip.h"
#endif

#include "tracy/Tracy.hpp"

#include "SDL3/SDL_surface.h"
#include <SDL3_image/SDL_image.h>

#include <array>
#include <chrono>
#include <charconv>
#include <map>
#include <cstdlib>
#include <execution>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace RTE;

ActivityMan::PendingCheckpoint::PendingCheckpoint() = default;
ActivityMan::PendingCheckpoint::~PendingCheckpoint() = default;
ActivityMan::PendingCheckpoint::PendingCheckpoint(PendingCheckpoint&&) noexcept = default;
ActivityMan::PendingCheckpoint& ActivityMan::PendingCheckpoint::operator=(PendingCheckpoint&&) noexcept = default;

ActivityMan::ActivityMan() {
	Clear();
}

ActivityMan::~ActivityMan() {
	Destroy();
}

void ActivityMan::Clear() {
	m_DefaultActivityType = "GATutorial";
	m_DefaultActivityName = "Tutorial Mission";
	m_Activity = nullptr;
	m_StartActivity = nullptr;
	PendingCheckpoint discarded = std::move(m_PendingCheckpoint);
	m_PendingCheckpoint = PendingCheckpoint{};
	m_RestartRestoresSnapshot = false;
	m_StartActivityResumed = false;
	m_SaveGameTask = std::shared_future<bool>();
	m_InActivity = false;
	m_ActivityNeedsRestart = false;
	m_ActivityNeedsResume = false;
	m_ResumingActivityFromPauseMenu = false;
	m_SkipPauseMenuWhenPausingActivity = false;
	m_LaunchIntoActivity = false;
	m_LaunchIntoEditor = false;
}

bool ActivityMan::Initialize() {
	if (IsSetToLaunchIntoEditor()) {
		// Evaluate LaunchIntoEditor before LaunchIntoActivity so it takes priority when both are set, otherwise it is ignored and editor is never launched.
		return SetStartEditorActivitySetToLaunchInto();
	} else if (IsSetToLaunchIntoActivity()) {
		m_ActivityNeedsRestart = true;
		return true;
	}
	return false;
}

bool ActivityMan::ForceAbortSave() {
	// Just a utility function we can call in the debugger quickwatch window to force an abort save to occur (great for force-saving the game when it crashes)
	// Throw ActivityMan::Instance().ForceAbortSave() into a quickwatch window and evaluate :)
	return SaveCurrentGame("AbortSave") && WaitForSaveGameTask();
}

bool ActivityMan::WaitForSaveGameTask() const {
	try {
		return !m_SaveGameTask.valid() || m_SaveGameTask.get();
	} catch (const std::exception&) {
		return false;
	}
}

// For some reason these aren't defined on Linux/MacOS... so
#define HACK_MZ_COMPRESS_METHOD_STORE 0
#define HACK_MZ_COMPRESS_LEVEL_FAST 2
#define HACK_MZ_COMPRESS_METHOD_DEFLATE 8

bool ActivityMan::SaveCurrentGame(const std::string& fileName) {
	WaitForSaveGameTask();
	std::promise<bool> refused;
	refused.set_value(false);
	m_SaveGameTask = refused.get_future().share();

	ZoneScopedN("Save Game");

	Scene* scene = g_SceneMan.GetScene();
	GAScripted* activity = dynamic_cast<GAScripted*>(GetActivity());

	if (fileName.empty() || !scene || !activity || activity->GetActivityState() == Activity::ActivityState::Over) {
		g_ConsoleMan.PrintString("ERROR: Cannot save when there's no game running, or the game is finished!");
		return false;
	}

	const auto saveStart = std::chrono::steady_clock::now();

	if (activity->RunLuaFunction("OnSave") < 0) return false;
	std::list<SceneObject*> saveObjects;
	g_MovableMan.GetAllActors(false, saveObjects);
	g_MovableMan.GetAllItems(false, saveObjects);
	g_MovableMan.GetAllParticles(false, saveObjects);
	std::vector<long> callbackObjects;
	for (SceneObject* object: saveObjects) {
		if (const auto* movable = dynamic_cast<MovableObject*>(object)) callbackObjects.push_back(movable->GetUniqueID());
	}
	for (long uid: callbackObjects) {
		if (MovableObject* object = g_MovableMan.FindObjectByUniqueID(uid)) object->OnSave();
	}
	g_MovableMan.CompleteQueuedMOIDDrawings();
	AudioMan::CheckpointRegistryScope captureSounds;
	const std::string runtimeGlobals = CaptureRuntimeGlobals();
	const std::string worldStructure = g_MovableMan.SaveWorldStructure();
	const std::string sceneRuntime = scene->SaveRuntimeCheckpoint();

	// Copy the layers in parallel with serialization, after all save callbacks.
	auto copyBitmaps = g_ThreadMan.GetBackgroundThreadPool().submit([scene]() {
		return scene->GetCopiedSceneLayerBitmaps();
	});
	struct AwaitBitmapCopy {
		std::future<std::vector<SceneLayerInfo>>& task;
		~AwaitBitmapCopy() { if (task.valid()) task.wait(); }
	} awaitBitmapCopy{copyBitmaps};

	// We need a copy of our scene, because we have to do some fixup to remove PLACEONLOAD items and only keep the current MovableMan state.
	std::unique_ptr<Scene> modifiableScene(dynamic_cast<Scene*>(scene->Clone()));

	// Delete any existing objects from our scene - we don't want to replace broken doors or repair any stuff when we load.
	modifiableScene->ClearPlacedObjectSet(Scene::PlacedObjectSets::PLACEONLOAD, true);

	// Become our own original preset, instead of being a copy of the Scene we got cloned from, so we don't still pick up the PlacedObjectSets from our parent when loading.
	modifiableScene->SetPresetName(fileName);
	modifiableScene->MigrateToModule(g_PresetMan.GetModuleID(c_UserScriptedSavesModuleName));
	modifiableScene->SetSavedGameInternal(true);

	// Make sure the terrain is also treated as an original preset, otherwise it will screw up if we save then load then save again, since it'll try to be a CopyOf of itself.
	modifiableScene->GetTerrain()->SetPresetName(fileName);
	modifiableScene->GetTerrain()->MigrateToModule(g_PresetMan.GetModuleID(c_UserScriptedSavesModuleName));

	// See our content files to point to our save game location. This won't actually save a file here- but it allows us to set these up as in-memory ContentFiles on load
	// Meaning that our loading code doesn't need to care about whether it's loading a savegame or a file- it just sees it as an already loaded, cached bitmap
	modifiableScene->GetTerrain()->GetContentFile().SetIsMemoryFile(true);
	modifiableScene->GetTerrain()->GetFGSceneLayer()->GetContentFile().SetIsMemoryFile(true);
	modifiableScene->GetTerrain()->GetBGSceneLayer()->GetContentFile().SetIsMemoryFile(true);

	modifiableScene->GetTerrain()->GetContentFile().SetDataPath(g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/Save Mat.png");
	modifiableScene->GetTerrain()->GetFGSceneLayer()->GetContentFile().SetDataPath(g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/Save FG.png");
	modifiableScene->GetTerrain()->GetBGSceneLayer()->GetContentFile().SetDataPath(g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/Save BG.png");

	for (int i = 0; i < Activity::MaxTeamCount; ++i) {
		SceneLayer* unseenLayer = modifiableScene->GetUnseenLayer(i);
		if (unseenLayer) {
			unseenLayer->GetContentFile().SetIsMemoryFile(true);
			unseenLayer->GetContentFile().SetDataPath(g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + std::format("/Save UST{}.png", i));
		}
	}

	std::unique_ptr<std::stringstream> iniStream = std::make_unique<std::stringstream>();

	// Block the main thread for a bit to let the Writer access the relevant data.
	auto writer = std::make_shared<Writer>(std::move(iniStream));
	Writer::SnapshotScope snapshotScope(*writer);
	writer->NewPropertyWithValue("Activity", activity);
	writer->NewPropertyWithValue("HasCheckpointStartActivity", m_StartActivity != nullptr);
	if (m_StartActivity) writer->NewPropertyWithValue("CheckpointStartActivity", m_StartActivity.get());
	writer->NewPropertyWithValue("RuntimeGlobals", base64_encode(runtimeGlobals, true));
	writer->NewPropertyWithValue("WorldStructure", base64_encode(worldStructure, true));
	writer->NewPropertyWithValue("SceneRuntime", base64_encode(sceneRuntime, true));

	// Pull all stuff from MovableMan into the Scene for saving, so existing Actors/ADoors are saved, without transferring ownership, so the game can continue.
	// TODO- copying may be faster, and lets us move all this actual writing into async
	struct BorrowedSceneObjects {
		Scene& scene;
		~BorrowedSceneObjects() { scene.ClearPlacedObjectSet(Scene::PlacedObjectSets::PLACEONLOAD, false); }
	} borrowedObjects{*modifiableScene};
	modifiableScene->RetrieveSceneObjects(false);

	writer->NewPropertyWithValue("OriginalScenePresetName", scene->GetPresetName());
	writer->NewPropertyWithValue("SimUpdateCount", g_TimerMan.GetSimUpdateCount());
	writer->NewPropertyWithValue("SimTimeTicks", g_TimerMan.GetSimTimeTicks());
	writer->NewPropertyWithValue("UniqueIDCounter", MovableObject::GetUniqueIDCounter());
	writer->NewPropertyWithValue("LuaStateCursor", g_LuaMan.GetScriptStateCursor());
	for (const auto& [tick, uid]: g_MovableMan.GetLockstepJoinQuarantine()) {
		writer->NewPropertyWithValue("LockstepJoinQuarantine", std::to_string(tick) + "|" + std::to_string(uid));
	}
	{
		std::vector<std::string> graphs;
		std::vector<std::string> luaProblems;
		if (!g_MovableMan.SerializeScriptGraphs(graphs, luaProblems)) {
			for (const std::string& problem: luaProblems) {
				g_ConsoleMan.PrintString("ERROR: the save cannot carry a script value: " + problem);
				std::cout << "[scriptgraph] save refused: " << problem << std::endl;
			}
			return false;
		}
		for (size_t index = 0; index < graphs.size(); ++index) {
			writer->NewPropertyWithValue("LuaStateGraph", std::to_string(index) + "|" + base64_encode(graphs[index], true));
		}
	}
	writer->NewPropertyWithValue("PlaceObjectsIfSceneIsRestarted", g_SceneMan.GetPlaceObjectsOnLoad());
	writer->NewPropertyWithValue("PlaceUnitsIfSceneIsRestarted", g_SceneMan.GetPlaceUnitsOnLoad());
	writer->NewPropertyWithValue("Scene", modifiableScene.get());

	// Save a small little file with index info (activity and original scene name) so we can display info in the samegame menu without needing to decompress and read through the entire zip
	std::unique_ptr<std::stringstream> indexStream = std::make_unique<std::stringstream>();
	auto indexWriter = std::make_shared<Writer>(std::move(indexStream));
	indexWriter->NewPropertyWithValue("ActivityName", activity->GetPresetName());
	indexWriter->NewPropertyWithValue("OriginalScenePresetName", scene->GetPresetName());

	auto sceneLayerInfos = std::make_shared<std::vector<SceneLayerInfo>>(copyBitmaps.get());
	const std::filesystem::path savePath = g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/" + fileName + ".ccsave";
	auto saveWriterData = [fileName, savePath, sceneLayerInfos, indexWriter, writer]() {
		struct PendingArchive {
			std::filesystem::path path;
			zipFile file = nullptr;
			~PendingArchive() {
				if (file) zipClose(file, nullptr);
				std::error_code ignored;
				std::filesystem::remove(path, ignored);
			}
		} archive{savePath.string() + ".tmp." + std::to_string(System::GetProcessID())};
		archive.file = zipOpen(archive.path.string().c_str(), APPEND_STATUS_CREATE);
		if (!archive.file) throw std::runtime_error("could not create temporary archive");
		const auto writeEntry = [&](const std::string& name, const void* data, size_t size, int method) {
			zip_fileinfo info{};
			const auto openEntry =
#ifdef SYSTEM_MINIZIP
			    zipOpenNewFileInZip64;
#else
			    zipOpenNewFileInZip_64;
#endif
			if (openEntry(archive.file, name.c_str(), &info, nullptr, 0, nullptr, 0, nullptr, method,
			                        HACK_MZ_COMPRESS_LEVEL_FAST, size >= 0xFFFFFFFFULL) != ZIP_OK) {
				throw std::runtime_error("could not open " + name + " in archive");
			}
			const char* bytes = static_cast<const char*>(data);
			while (size > 0) {
				const auto count = static_cast<unsigned int>(std::min<size_t>(size, std::numeric_limits<unsigned int>::max()));
				if (zipWriteInFileInZip(archive.file, bytes, count) != ZIP_OK) throw std::runtime_error("could not write " + name);
				bytes += count;
				size -= count;
			}
			if (zipCloseFileInZip(archive.file) != ZIP_OK) throw std::runtime_error("could not finish " + name);
		};
		const std::string_view mainText = static_cast<std::stringstream*>(writer->GetStream())->view();
		const std::string_view indexText = static_cast<std::stringstream*>(indexWriter->GetStream())->view();
		writeEntry("Index.ini", indexText.data(), indexText.size(), HACK_MZ_COMPRESS_METHOD_STORE);
		writeEntry("Save.ini", mainText.data(), mainText.size(), HACK_MZ_COMPRESS_METHOD_DEFLATE);

		std::vector<std::vector<unsigned char>> pngData(sceneLayerInfos->size());
		std::for_each(std::execution::par, sceneLayerInfos->begin(), sceneLayerInfos->end(), [&](const SceneLayerInfo& layerInfo) {
			const size_t index = &layerInfo - sceneLayerInfos->data();
			try {
				ContentFile::EncodeIndexedPNG(layerInfo.bitmap.get(), pngData[index]);
			} catch (const std::exception&) {
				pngData[index].clear();
			}
		});
		for (size_t i = 0; i < pngData.size(); ++i) {
			const auto& png = pngData[i];
			const std::string name = "Save " + (*sceneLayerInfos)[i].name + ".png";
			if (png.empty()) throw std::runtime_error("could not encode " + name);
			writeEntry(name, png.data(), png.size(), HACK_MZ_COMPRESS_METHOD_STORE);
		}
		const int closed = zipClose(archive.file, fileName.c_str());
		archive.file = nullptr;
		if (closed != ZIP_OK) throw std::runtime_error("could not finish archive");
		std::filesystem::rename(archive.path, savePath);
	};

	const long long saveMainMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - saveStart).count();

	// Parse-cost probe: bounds the re-read side of a fast in-place restore (parse only, no apply).
	if (const char* parseProbe = std::getenv("CC_SNAP_PARSE_PROBE"); parseProbe && parseProbe[0] == '1') {
		const std::string snapshotText = static_cast<std::stringstream*>(writer->GetStream())->str();
		const auto parseStart = std::chrono::steady_clock::now();
		Reader probeReader(std::make_unique<std::istringstream>(snapshotText), "SnapParseProbe", true, nullptr, true);
		long long parsedProps = 0;
		while (!probeReader.ReadPropName().empty()) {
			probeReader.ReadPropValue();
			++parsedProps;
		}
		const long long parseMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - parseStart).count();
		std::cout << "[snapbench] parse_ms=" << parseMs << " props=" << parsedProps << " bytes=" << snapshotText.size() << std::endl;
	}

	m_SaveGameTask = g_ThreadMan.GetBackgroundThreadPool().submit([saveWriterData, saveMainMs, fileName]() {
		const auto asyncStart = std::chrono::steady_clock::now();
		bool saved = false;
		try {
			saveWriterData();
			saved = true;
			g_ConsoleMan.PrintString("SYSTEM: Game saved to \"" + fileName + "\"!");
		} catch (const std::exception& error) {
			g_ConsoleMan.PrintString("ERROR: Could not save game \"" + fileName + "\": " + error.what());
		}
		const long long asyncMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - asyncStart).count();
		std::cout << "[snapbench] save main_ms=" << saveMainMs << " zip_io_ms=" << asyncMs << " saved=" << saved << std::endl;
		return saved;
	}).share();

	return true;
}

bool ActivityMan::ReadSavedGame(const std::string& fileName, PendingCheckpoint& out) {
	WaitForSaveGameTask();
	const std::string modulePath = g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName);
	const std::string filePath = modulePath + "/" + fileName;
	try {
		SaveGameArchive archive(filePath + ".ccsave");
		std::string text;
		archive.ReadEntry("Save.ini", text);
		if (text.empty() || text.find('\0') != std::string::npos) throw std::runtime_error("empty or invalid Save.ini");

		using Image = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
		std::vector<std::pair<std::string, Image>> images;
		for (int layer = 0; layer < 3 + Activity::MaxTeamCount; ++layer) {
			const std::string name = layer == 0 ? "Save Mat.png" : layer == 1 ? "Save FG.png" : layer == 2 ? "Save BG.png" : std::format("Save UST{}.png", layer - 3);
			std::string bytes;
			if (!archive.ReadEntry(name, bytes, layer < 3)) continue;
			std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)> stream(SDL_IOFromConstMem(bytes.data(), bytes.size()), SDL_CloseIO);
			Image image(stream ? IMG_LoadPNG_IO(stream.get()) : nullptr, SDL_DestroySurface);
			if (!image || image->w <= 0 || image->h <= 0) throw std::runtime_error("invalid image " + name);
			std::unique_ptr<SDL_Palette, decltype(&SDL_DestroyPalette)> palette(ContentFile::DefaultPaletteToSDL(), SDL_DestroyPalette);
			if (!palette) throw std::runtime_error("could not allocate image palette");
			if (image->format != SDL_PIXELFORMAT_INDEX8) {
				image.reset(SDL_ConvertSurfaceAndColorspace(image.get(), SDL_PIXELFORMAT_INDEX8, palette.get(), SDL_COLORSPACE_UNKNOWN, 0));
				if (!image) throw std::runtime_error("could not convert " + name);
			} else if (!SDL_SetSurfacePalette(image.get(), palette.get())) {
				throw std::runtime_error("could not set palette for " + name);
			}
			if (layer > 0 && layer < 3 && (image->w != images.front().second->w || image->h != images.front().second->h)) {
				throw std::runtime_error("terrain image dimensions do not match");
			}
			images.emplace_back(name, std::move(image));
		}

		auto stagedImages = std::make_unique<ContentFile::MemoryPNGScope>();
		for (auto& [name, image]: images) {
			if (!stagedImages->Add(modulePath + "/" + name, image.get())) throw std::runtime_error("could not stage " + name);
			image.release();
		}
		Reader reader(std::make_unique<std::istringstream>(text), filePath + "/Save.ini", true, nullptr, true);
		reader.SetCheckpoint(true);
		reader.SetThrowOnError(true);
		reader.SetSkipIncludes(true);
		auto scene = std::make_unique<Scene>();
		auto activity = std::make_unique<GAScripted>();
		std::unique_ptr<Activity> startActivity;
		bool hasStartActivity = false, startActivityDeclared = false;
		std::string originalScenePresetName = fileName;
		bool placeObjects = true, placeUnits = true, hasActivity = false, hasScene = false;
		long long simUpdateCount = -1, simTimeTicks = 0;
		long uniqueIDCounter = -1;
		int luaStateCursor = -1;
		std::vector<std::pair<uint64_t, long int>> joinQuarantine;
		std::map<size_t, std::string> graphs;
		std::string runtimeGlobals, worldStructure, sceneRuntime;
		while (reader.NextProperty()) {
			const std::string propName = reader.ReadPropName();
			if (propName == "Activity") {
				if (hasActivity || static_cast<Serializable*>(activity.get())->Create(reader) < 0) throw std::runtime_error("invalid Activity");
				hasActivity = true;
			} else if (propName == "HasCheckpointStartActivity") {
				if (startActivityDeclared) throw std::runtime_error("duplicate start activity declaration");
				reader >> hasStartActivity;
				startActivityDeclared = true;
			} else if (propName == "CheckpointStartActivity") {
				if (startActivity) throw std::runtime_error("duplicate start activity");
				std::unique_ptr<Entity> value(g_PresetMan.ReadReflectedPreset(reader));
				if (!dynamic_cast<Activity*>(value.get())) throw std::runtime_error("invalid start activity");
				startActivity.reset(static_cast<Activity*>(value.release()));
			} else if (propName == "OriginalScenePresetName") {
				reader >> originalScenePresetName;
			} else if (propName == "SimUpdateCount") {
				reader >> simUpdateCount;
			} else if (propName == "WorldStructure") {
				worldStructure = base64_decode(reader.ReadPropValue());
				if (!g_MovableMan.LoadWorldStructure(worldStructure, true)) throw std::runtime_error("invalid world structure");
			} else if (propName == "RuntimeGlobals") {
				runtimeGlobals = base64_decode(reader.ReadPropValue());
				if (!RestoreRuntimeGlobals(runtimeGlobals, true)) throw std::runtime_error("invalid runtime globals");
			} else if (propName == "SceneRuntime") {
				sceneRuntime = base64_decode(reader.ReadPropValue());
			} else if (propName == "SimTimeTicks") {
				reader >> simTimeTicks;
			} else if (propName == "UniqueIDCounter") {
				reader >> uniqueIDCounter;
				// Temporary reader/graph clones must never borrow an incoming object's saved ID.
				MovableObject::PinUniqueIDCounter(std::max(MovableObject::GetUniqueIDCounter(), uniqueIDCounter));
			} else if (propName == "LuaStateCursor") {
				reader >> luaStateCursor;
			} else if (propName == "LockstepJoinQuarantine") {
				const std::string entry = reader.ReadPropValue();
				const size_t bar = entry.find('|');
				uint64_t tick = 0;
				long uid = 0;
				if (bar == std::string::npos) throw std::runtime_error("invalid join quarantine");
				const auto parsedTick = std::from_chars(entry.data(), entry.data() + bar, tick);
				const auto parsedUID = std::from_chars(entry.data() + bar + 1, entry.data() + entry.size(), uid);
				if (parsedTick.ec != std::errc() || parsedTick.ptr != entry.data() + bar || parsedUID.ec != std::errc() || parsedUID.ptr != entry.data() + entry.size() || uid <= 0) {
					throw std::runtime_error("invalid join quarantine");
				}
				joinQuarantine.emplace_back(tick, uid);
			} else if (propName == "LuaStateGraph") {
				std::string entry;
				reader >> entry;
				const size_t bar = entry.find('|');
				size_t index = 0;
				if (bar != std::string::npos) {
					const auto parsed = std::from_chars(entry.data(), entry.data() + bar, index);
					if (parsed.ec != std::errc() || parsed.ptr != entry.data() + bar) throw std::runtime_error("invalid script state index");
				}
				if (!graphs.emplace(index, base64_decode(bar == std::string::npos ? entry : entry.substr(bar + 1))).second) {
					throw std::runtime_error("duplicate script state index");
				}
			} else if (propName == "PlaceObjectsIfSceneIsRestarted") {
				reader >> placeObjects;
			} else if (propName == "PlaceUnitsIfSceneIsRestarted") {
				reader >> placeUnits;
			} else if (propName == "Scene") {
				if (hasScene || static_cast<Serializable*>(scene.get())->Create(reader) < 0) throw std::runtime_error("invalid Scene");
				hasScene = true;
			} else {
				reader.ReadPropValue();
			}
		}
		if ((startActivityDeclared && hasStartActivity != (startActivity != nullptr)) || (!startActivityDeclared && startActivity)) throw std::runtime_error("incomplete start activity checkpoint");
		if (!hasActivity || !hasScene || !scene->GetTerrain()) throw std::runtime_error("missing Activity, Scene or terrain");
		if (!sceneRuntime.empty() && !scene->LoadRuntimeCheckpoint(sceneRuntime, true)) throw std::runtime_error("invalid scene runtime");
		const auto checkLayer = [&](SceneLayer* layer, const std::string& name) {
			if (!layer || layer->GetContentFile().GetDataPath() != modulePath + "/" + name ||
			    std::none_of(images.begin(), images.end(), [&](const auto& image) { return image.first == name; })) {
				throw std::runtime_error("missing or invalid layer " + name);
			}
		};
		checkLayer(scene->GetTerrain(), "Save Mat.png");
		checkLayer(scene->GetTerrain()->GetFGSceneLayer(), "Save FG.png");
		checkLayer(scene->GetTerrain()->GetBGSceneLayer(), "Save BG.png");
		for (int team = 0; team < Activity::MaxTeamCount; ++team) {
			if (auto* unseen = scene->GetUnseenLayer(team)) checkLayer(unseen, std::format("Save UST{}.png", team));
		}
		if (!graphs.empty() && graphs.rbegin()->first != graphs.size() - 1) throw std::runtime_error("missing script state index");
		std::vector<std::string> scriptGraphs;
		for (auto& [index, graph]: graphs) scriptGraphs.emplace_back(std::move(graph));
		if (scriptGraphs.size() > g_LuaMan.GetThreadedScriptStates().size() + 1) throw std::runtime_error("unsupported script state count");
		std::vector<std::string> graphProblems;
		for (size_t index = 0; index < scriptGraphs.size(); ++index) {
			if (!scriptGraphs[index].empty()) g_LuaMan.GetStateByIndex(static_cast<int>(index)).ValidateScriptGraph(scriptGraphs[index], graphProblems);
		}
		if (!graphProblems.empty()) throw std::runtime_error(graphProblems.front());

		out.scene = std::move(scene);
		out.activity = std::move(activity);
		out.startActivity = std::move(startActivity);
		out.hasStartActivity = startActivityDeclared;
		out.restartPreset = std::move(originalScenePresetName);
		out.restartObjects = placeObjects;
		out.restartUnits = placeUnits;
		out.simUpdateCount = simUpdateCount;
		out.simTimeTicks = simTimeTicks;
		out.uniqueIDCounter = uniqueIDCounter;
		out.luaStateCursor = luaStateCursor;
		out.joinQuarantine = std::move(joinQuarantine);
		out.scriptGraphs = std::move(scriptGraphs);
		out.runtimeGlobals = std::move(runtimeGlobals);
		out.worldStructure = std::move(worldStructure);
		out.sceneRuntime = std::move(sceneRuntime);
		stagedImages->SetActive(false);
		out.images = std::move(stagedImages);
		return true;
	} catch (const std::exception& error) {
		const std::string message = "Could not load game \"" + fileName + "\": " + error.what();
		g_ConsoleMan.PrintString("ERROR: " + message);
		RTEError::ShowMessageBox(message);
		return false;
	}
}

bool ActivityMan::RunSaveCallbacksSelfTest() {
	auto* activity = dynamic_cast<GAScripted*>(GetActivity());
	if (!activity) return false;
	const std::string activityClass = activity->GetLuaClassName();
	LuaStateWrapper& scriptState = g_LuaMan.GetMasterScriptState();
	if (scriptState.RunScriptFile("UserScenes.rte/mod_save_callbacks.lua", true, false) < 0 ||
	    scriptState.RunScriptString(activityClass + ".OnSave = SaveActivity") < 0) return false;
	activity->RefreshActivityFunctions();
	std::list<SceneObject*> actors;
	g_MovableMan.GetAllActors(false, actors);
	if (actors.empty()) return false;
	const size_t initialActors = actors.size();
	auto* actor = dynamic_cast<Actor*>(actors.front());
	if (!actor || actor->LoadScript(g_PresetMan.GetFullModulePath("UserScenes.rte/mod_save_callbacks.lua")) < 0) return false;
	const long actorUID = actor->GetUniqueID();
	const int x = static_cast<int>(actor->GetPos().GetX()), y = static_cast<int>(actor->GetPos().GetY());
	rectfill(g_SceneMan.GetTerrain()->GetBitmap(), x - 30, y - 30, x + 30, y + 30, 30);
	if (!SaveCurrentGame("save_callbacks") || !WaitForSaveGameTask()) return false;
	const Vector acceleration = g_SceneMan.GetScene()->GetGlobalAcc();
	const auto expectedImages = g_SceneMan.GetScene()->GetCopiedSceneLayerBitmaps();
	actors.clear();
	g_MovableMan.GetAllActors(false, actors);
	const size_t expectedActors = actors.size();
	const bool callback = actor->GetNumberValue("save_callback_count") == 1 && acceleration == Vector(1, 23) &&
	                      g_SceneMan.GetScene()->HasArea("Save callback area") && expectedActors == initialActors + 1;
	const bool loaded = LoadAndLaunchGame("save_callbacks");
	bool images = loaded;
	if (loaded) {
		const auto actualImages = g_SceneMan.GetScene()->GetCopiedSceneLayerBitmaps();
		images = actualImages.size() == expectedImages.size();
		for (size_t i = 0; images && i < actualImages.size(); ++i) {
			const BITMAP* expected = expectedImages[i].bitmap.get();
			const BITMAP* actual = actualImages[i].bitmap.get();
			images = expected->w == actual->w && expected->h == actual->h;
			for (int row = 0; images && row < actual->h; ++row) images = std::memcmp(expected->line[row], actual->line[row], actual->w) == 0;
		}
	}
	actors.clear();
	g_MovableMan.GetAllActors(false, actors);
	const auto* restoredActor = g_MovableMan.FindObjectByUniqueID(actorUID);
	const bool scene = loaded && g_SceneMan.GetScene()->GetGlobalAcc() == acceleration && g_SceneMan.GetScene()->HasArea("Save callback area") &&
	                   g_SceneMan.GetScene()->HasArea("Activity save callback area");
	const bool objects = loaded && actors.size() == expectedActors && restoredActor && restoredActor->GetNumberValue("save_callback_count") == 1;
	const bool activityState = loaded && GetActivity()->GetTeamFunds(Activity::TeamOne) == 357 &&
	                           scriptState.RunScriptString("assert(" + activityClass + ".save_callback_count == 1)") == 0;
	const bool savedAgain = loaded && SaveCurrentGame("save_callbacks_again") && WaitForSaveGameTask();
	const bool repeated = savedAgain && scriptState.RunScriptString("assert(" + activityClass + ".save_callback_count == 2)") == 0 &&
	                      LoadAndLaunchGame("save_callbacks_again") && scriptState.RunScriptString("assert(" + activityClass + ".save_callback_count == 2)") == 0;
	const bool passed = callback && loaded && images && scene && objects && activityState && repeated;
	std::cout << "[save-callback-selftest] " << (passed ? "PASS" : "FAIL") << " callback=" << callback << " loaded=" << loaded
	          << " images=" << images << " scene=" << scene << " objects=" << objects << " activity=" << activityState << " repeated=" << repeated << std::endl;
	return passed;
}

bool ActivityMan::RunGlobalCallbacksSelfTest() {
	const auto findScript = [this]() -> GlobalScript* {
		if (auto* activity = dynamic_cast<GAScripted*>(GetActivity())) {
			for (GlobalScript* script: activity->GetGlobalScripts()) {
				if (script->GetPresetName() == "Checkpoint Global") return script;
			}
		}
		return nullptr;
	};
	GlobalScript* script = findScript();
	const auto* craftPreset = g_PresetMan.GetEntityPreset("ACDropShip", "Dropship MK1", "Base.rte");
	const auto* scriptPreset = g_PresetMan.GetEntityPreset("GlobalScript", "Checkpoint Global", "UserScenes.rte");
	if (!script || !craftPreset || !scriptPreset) return false;
	auto* craft = dynamic_cast<ACraft*>(craftPreset->Clone());
	craft->SetPos(Vector(500, 100));
	craft->SetTeam(Activity::TeamOne);
	craft->SetPinStrength(10000);
	g_MovableMan.AddActor(craft);
	const long craftUID = craft->GetUniqueID();
	LuaStateWrapper& state = g_LuaMan.GetMasterScriptState();
	state.SetTempEntity(craft);
	if (state.RunScriptString("CheckpointGlobalScript.expectedCraft = ToACraft(LuaMan.TempEntity); CheckpointGlobalScript:ResetEvents()") < 0) return false;
	{
		std::unique_ptr<GlobalScript> unstarted(dynamic_cast<GlobalScript*>(scriptPreset->Clone()));
		std::unique_ptr<ACraft> unregistered(dynamic_cast<ACraft*>(craftPreset->Clone()));
		unstarted->HandleCraftEnteringOrbit(craft);
		script->HandleCraftEnteringOrbit(nullptr);
		script->HandleCraftEnteringOrbit(unregistered.get());
		script->SetActive(false);
		script->Pause(true);
		script->HandleCraftEnteringOrbit(craft);
		script->SetActive(true);
	}
	const bool guards = state.RunScriptString("CheckpointGlobalScript:VerifyEvents(0)") == 0;
	state.RunScriptString("CheckpointGlobalScript:ResetEvents()");
	const auto dispatch = [&](int expected) {
		GlobalScript* current = findScript();
		auto* currentCraft = dynamic_cast<ACraft*>(g_MovableMan.FindObjectByUniqueID(craftUID));
		if (!current || !currentCraft) return false;
		const bool pause = current->Pause(true) == 0 && current->Pause(false) == 0;
		GetActivity()->HandleCraftEnteringOrbit(currentCraft);
		const bool ended = current->End() == 0;
		return state.RunScriptString("CheckpointGlobalScript:VerifyEvents(" + std::to_string(expected) + ")") == 0 && pause && ended;
	};
	const bool initial = dispatch(1111);
	std::vector<std::string> graphs, problems;
	const bool captured = g_MovableMan.SerializeScriptGraphs(graphs, problems);
	const bool saved = SaveCurrentGame("global_callbacks") && WaitForSaveGameTask();
	const bool advanced = dispatch(2222);
	std::string error;
	const bool memory = captured && advanced && g_MovableMan.RestoreScriptGraphs(graphs, &error) && dispatch(2222);
	const bool file = saved && LoadAndLaunchGame("global_callbacks") && dispatch(2222);
	const bool passed = guards && initial && memory && file;
	std::cout << "[global-callback-selftest] " << (passed ? "PASS" : "FAIL") << " guards=" << guards << " initial=" << initial
	          << " memory=" << memory << " file=" << file << " error=" << error << std::endl;
	return passed;
}

bool ActivityMan::RunLoadSelfTest(const std::string& fileName, bool expectLoaded) {
	if (!LoadGameToRestart("load_seed")) return false;
	const Scene* stagedScene = m_PendingCheckpoint.scene.get();
	const Activity* stagedActivity = m_PendingCheckpoint.activity.get();
	const Activity* configuredStart = m_StartActivity.get();
	m_PendingCheckpoint.images->SetActive(true);
	const Activity* runningActivity = m_Activity.get();
	const auto stagedTick = m_PendingCheckpoint.simUpdateCount;
	const auto stagedTime = m_PendingCheckpoint.simTimeTicks;
	const auto stagedUID = m_PendingCheckpoint.uniqueIDCounter;
	const auto stagedCursor = m_PendingCheckpoint.luaStateCursor;
	const auto stagedGraphs = m_PendingCheckpoint.scriptGraphs;
	const auto liveUID = MovableObject::GetUniqueIDCounter();
	const auto liveCursor = g_LuaMan.GetScriptStateCursor();
	ContentFile material((g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/Save Mat.png").c_str());
	const int firstPixel = getpixel(material.GetAsBitmap(), 0, 0);
	const bool loaded = LoadGameToRestart(fileName);
	const bool preserved = loaded || (m_PendingCheckpoint.scene.get() == stagedScene && m_PendingCheckpoint.activity.get() == stagedActivity && m_StartActivity.get() == configuredStart &&
		m_Activity.get() == runningActivity && m_PendingCheckpoint.simUpdateCount == stagedTick && m_PendingCheckpoint.simTimeTicks == stagedTime &&
		m_PendingCheckpoint.uniqueIDCounter == stagedUID && m_PendingCheckpoint.luaStateCursor == stagedCursor && m_PendingCheckpoint.scriptGraphs == stagedGraphs &&
		MovableObject::GetUniqueIDCounter() == liveUID && g_LuaMan.GetScriptStateCursor() == liveCursor &&
		getpixel(material.GetAsBitmap(), 0, 0) == firstPixel && m_RestartRestoresSnapshot && m_ActivityNeedsRestart);
	const bool restarted = RestartActivity();
	const bool terrain = restarted && getpixel(g_SceneMan.GetTerrain()->GetBitmap(), 0, 0) == (expectLoaded ? 28 : firstPixel);
	const bool passed = loaded == expectLoaded && preserved && restarted && terrain;
	std::cout << "[load-selftest] " << (passed ? "PASS" : "FAIL") << " loaded=" << loaded << " preserved=" << preserved
	          << " restarted=" << restarted << " terrain=" << terrain << std::endl;
	return passed;
}

bool ActivityMan::LoadAndLaunchGame(const std::string& fileName) {
	PendingCheckpoint previous = std::move(m_PendingCheckpoint);
	const bool previousRestore = m_RestartRestoresSnapshot, previousRestart = m_ActivityNeedsRestart;
	m_PendingCheckpoint = PendingCheckpoint{};
	if (!LoadGameToRestart(fileName) || !RestartActivity()) {
		PendingCheckpoint rejected = std::move(m_PendingCheckpoint);
		m_PendingCheckpoint = std::move(previous);
		m_RestartRestoresSnapshot = previousRestore;
		m_ActivityNeedsRestart = previousRestart;
		return false;
	}
	g_ConsoleMan.PrintString("SYSTEM: Game \"" + fileName + "\" loaded!");
	return true;
}

void ActivityMan::RemoveSavedGame(const std::string& fileName) const {
	std::error_code ignored;
	std::filesystem::remove(g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/" + fileName + ".ccsave", ignored);
}

bool ActivityMan::LoadGameToRestart(const std::string& fileName) {
	MovableMan::ConstructionRegistryScope registryScope;
	MovableObject::ScriptLoadDeferralScope scriptScope;
	struct RestoreConstructionGlobals {
		RandomGenerator sim = g_SimRNG;
		RandomGenerator render = g_RenderRNG;
		bool restoring = g_MovableMan.IsRestoringSnapshot();
		~RestoreConstructionGlobals() { g_SimRNG = sim; g_RenderRNG = render; g_MovableMan.SetRestoringSnapshot(restoring); }
	} constructionGlobals;
	PendingCheckpoint candidate;
	const auto readStart = std::chrono::steady_clock::now();
	const long uidCounter = MovableObject::GetUniqueIDCounter();
	const int luaStateCursor = g_LuaMan.GetScriptStateCursor();
	const bool wasRestoring = g_MovableMan.IsRestoringSnapshot();
	g_MovableMan.SetRestoringSnapshot(true);
	const bool read = ReadSavedGame(fileName, candidate);
	g_MovableMan.SetRestoringSnapshot(wasRestoring);
	if (!read) {
		MovableObject::PinUniqueIDCounter(uidCounter);
		g_LuaMan.SetScriptStateCursor(luaStateCursor);
		return false;
	}
	m_RestartRestoresSnapshot = true;
	std::cout << "[snapbench] read_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - readStart).count() << std::endl;

	candidate.scene->SetPresetName(candidate.restartPreset);
	candidate.soundRegistrations = registryScope.GetStagedSoundRegistrations();
	PendingCheckpoint previous = std::move(m_PendingCheckpoint);
	m_PendingCheckpoint = std::move(candidate);
	SetRestartActivity(true);

	g_ConsoleMan.PrintString("SYSTEM: Game \"" + fileName + "\" staged for restart!");

	return true;
}

void ActivityMan::SetStartActivity(Activity* newActivity) {
	RTEAssert(newActivity, "Trying to replace an activity with a null one!");
	m_StartActivity.reset(newActivity);
	m_StartActivityResumed = false;
}

void ActivityMan::SetStartTutorialActivity() {
	SetStartActivity(dynamic_cast<Activity*>(g_PresetMan.GetEntityPreset("GATutorial", "Tutorial Mission")->Clone()));
	if (GameActivity* gameActivity = dynamic_cast<GameActivity*>(GetStartActivity())) {
		gameActivity->SetStartingGold(10000);
	}
	g_SceneMan.SetSceneToLoad("Tutorial Bunker");
}

void ActivityMan::SetStartEditorActivity(const std::string_view& editorToLaunch) {
	std::unique_ptr<EditorActivity> editorActivityToStart = nullptr;

	if (editorToLaunch == "ActorEditor") {
		editorActivityToStart = std::make_unique<ActorEditor>();
	} else if (editorToLaunch == "GibEditor") {
		editorActivityToStart = std::make_unique<GibEditor>();
	} else if (editorToLaunch == "SceneEditor") {
		editorActivityToStart = std::make_unique<SceneEditor>();
	} else if (editorToLaunch == "AreaEditor") {
		editorActivityToStart = std::make_unique<AreaEditor>();
	} else if (editorToLaunch == "AssemblyEditor") {
		editorActivityToStart = std::make_unique<AssemblyEditor>();
	}
	if (editorActivityToStart) {
		if (g_MetaMan.GameInProgress()) {
			g_MetaMan.EndGame();
		}
		g_SceneMan.SetSceneToLoad("Editor Scene");
		editorActivityToStart->Create();
		editorActivityToStart->SetEditorMode(EditorActivity::LOADDIALOG);
		SetStartActivity(editorActivityToStart.release());
		m_ActivityNeedsRestart = true;
	} else {
		RTEAbort("Failed to instantiate the " + std::string(editorToLaunch) + " Activity!");
	}
}

bool ActivityMan::SetStartEditorActivitySetToLaunchInto() {
	std::array<std::string_view, 5> validEditorNames = {"ActorEditor", "GibEditor", "SceneEditor", "AreaEditor", "AssemblyEditor"};

	if (std::find(validEditorNames.begin(), validEditorNames.end(), m_EditorToLaunch) != validEditorNames.end()) {
		// Force mouse + keyboard with default mapping so we won't need to change manually if player 1 is set to keyboard only or gamepad.
		g_UInputMan.GetControlScheme(Players::PlayerOne)->SetDevice(InputDevice::DEVICE_MOUSE_KEYB);
		g_UInputMan.GetControlScheme(Players::PlayerOne)->SetPreset(InputScheme::InputPreset::PresetMouseWASDKeys);
		SetStartEditorActivity(m_EditorToLaunch);
		return true;
	} else {
		g_ConsoleMan.PrintString("ERROR: Invalid editor name passed into \"-editor\" argument!");
		g_ConsoleMan.SetEnabled(true);
		m_LaunchIntoEditor = false;
		return false;
	}
}

int ActivityMan::StartActivity(Activity* activity) {
	RTEAssert(activity, "Trying to start a null activity!");

	g_ThreadMan.GetPriorityThreadPool().wait_for_tasks();
	g_ThreadMan.GetBackgroundThreadPool().wait_for_tasks();
	g_LuaMan.ResetPathCallbacks(true);

	m_StartActivity.reset(activity);
	m_StartActivityResumed = false;
	m_Activity.reset(dynamic_cast<Activity*>(m_StartActivity->Clone()));

	if (!g_MovableMan.IsRestoringSnapshot()) g_MusicMan.ResetMusicState();

	m_Activity->SetupPlayers();
	int error = m_Activity->Start();

	if (error >= 0)
		g_ConsoleMan.PrintString("SYSTEM: Activity \"" + m_Activity->GetPresetName() + "\" was successfully started");
	else {
		g_ConsoleMan.PrintString("ERROR: Activity \"" + m_Activity->GetPresetName() + "\" was NOT started due to errors!");
		m_Activity->SetActivityState(Activity::HasError);
		return error;
	}

	// Close the console in case it was open by the player or because of a previous Activity error.
	g_ConsoleMan.SetEnabled(false);

	m_ActivityNeedsResume = !g_MovableMan.IsRestoringSnapshot();
	m_InActivity = true;

	if (!g_MovableMan.IsRestoringSnapshot()) {
		g_PostProcessMan.ClearScenePostEffects();
		g_FrameMan.ClearScreenText();
		g_UInputMan.SetMouseValueMagnitude(0, g_UInputMan.MouseUsedByPlayer());
		g_AudioMan.PauseIngameSounds(false);
	}

	g_PerformanceMan.ResetPerformanceTimings();

	return error;
}

int ActivityMan::StartActivity(const std::string& className, const std::string& presetName) {
	if (const Entity* entity = g_PresetMan.GetEntityPreset(className, presetName)) {
		Activity* newActivity = dynamic_cast<Activity*>(entity->Clone());
		if (GameActivity* newActivityAsGameActivity = dynamic_cast<GameActivity*>(newActivity)) {
			newActivityAsGameActivity->SetStartingGold(newActivityAsGameActivity->GetDefaultGoldMediumDifficulty());
			if (newActivityAsGameActivity->GetStartingGold() <= 0) {
				newActivityAsGameActivity->SetStartingGold(static_cast<int>(newActivityAsGameActivity->GetTeamFunds(0)));
			} else {
				newActivityAsGameActivity->SetTeamFunds(static_cast<float>(newActivityAsGameActivity->GetStartingGold()), 0);
			}
		}
		return StartActivity(newActivity);
	} else {
		g_ConsoleMan.PrintString("ERROR: Couldn't find the " + className + " named " + presetName + " to start! Has it been defined?");
		return -1;
	}
}

void ActivityMan::PauseActivity(bool pause, bool skipPauseMenu) {
	if (!m_Activity) {
		g_ConsoleMan.PrintString("ERROR: No Activity to pause!");
		return;
	}

	if (pause == m_Activity->IsPaused()) {
		return;
	}

	m_Activity->SetPaused(pause);
	m_InActivity = !pause;
	m_ResumingActivityFromPauseMenu = false;
	m_SkipPauseMenuWhenPausingActivity = skipPauseMenu;

	g_AudioMan.PauseIngameSounds(pause);
	g_AudioMan.SetMusicMuffledState(pause);

	if (!pause) {
		g_MusicMan.EndInterruptingMusic();
	}

	g_ConsoleMan.PrintString("SYSTEM: Activity \"" + m_Activity->GetPresetName() + "\" was " + (pause ? "paused" : "resumed"));
}

void ActivityMan::ResumeActivity() {
	if (GetActivity()->GetActivityState() != Activity::NotStarted) {
		m_InActivity = true;
		m_ActivityNeedsResume = false;

		std::vector<int> humanPlayers;
		for (int player = 0; player < MaxPlayerCount; player++) {
			if (m_Activity->PlayerHuman(player)) {
				humanPlayers.push_back(player);
			}
		}
		g_UInputMan.CheckMultiMouseKeyboardEnabled(humanPlayers);
		PauseActivity(false);
		g_TimerMan.PauseSim(false);
		g_PerformanceMan.ResetPerformanceTimings();
	}
}

bool ActivityMan::RestartActivityCandidate() {
	m_ActivityNeedsRestart = false;
	const auto restartStart = std::chrono::steady_clock::now();
	g_ConsoleMan.PrintString("SYSTEM: Activity was reset!");

	if (!m_RestartRestoresSnapshot) g_AudioMan.StopAll();
	g_MovableMan.PurgeAllMOs();
	if (m_RestartRestoresSnapshot && !m_PendingCheckpoint.scriptGraphs.empty()) {
		for (size_t index = 0; index < m_PendingCheckpoint.scriptGraphs.size(); ++index) {
			if (!m_PendingCheckpoint.scriptGraphs[index].empty()) g_LuaMan.GetStateByIndex(static_cast<int>(index)).ReleaseScriptOwnedObjects();
		}
	}
	// Have to reset TimerMan before creating anything else because all timers are reset against it.
	g_TimerMan.ResetTime();
	const bool restoresSnapshot = m_RestartRestoresSnapshot;
	m_RestartRestoresSnapshot = false;
	if (restoresSnapshot && m_PendingCheckpoint.simUpdateCount >= 0) {
		g_TimerMan.RewindSimTo(m_PendingCheckpoint.simUpdateCount, m_PendingCheckpoint.simTimeTicks);
	}
	g_MovableMan.SetRestoringSnapshot(restoresSnapshot);

	// TODO: Deal with GUI resetting here!$@#") // Figure out what the hell this is about.

	int activityStarted;
	if (m_StartActivity) {
		// Need to pass in a clone of the activity because the original will be deleted and re-set during StartActivity.
		Activity* startActivityToUse = dynamic_cast<Activity*>(m_StartActivity->Clone());
		// A loaded save resumes mid-state; a fresh restart re-runs the scripts' spawn-time setup.
		if (m_StartActivityResumed) {
			m_StartActivityResumed = false;
		} else {
			startActivityToUse->SetActivityState(Activity::ActivityState::NotStarted);
		}
		activityStarted = StartActivity(startActivityToUse);
	} else {
		activityStarted = StartActivity(m_DefaultActivityType, m_DefaultActivityName);
	}
	if (restoresSnapshot && activityStarted >= 0 && !m_PendingCheckpoint.sceneRuntime.empty() && (!g_SceneMan.GetScene() || !g_SceneMan.GetScene()->LoadRuntimeCheckpoint(m_PendingCheckpoint.sceneRuntime))) {
		g_ConsoleMan.PrintString("ERROR: the saved scene runtime did not restore"); activityStarted = -1;
	}
	if (restoresSnapshot && activityStarted >= 0 && !m_Activity->ApplyPendingCheckpoint()) {
		g_ConsoleMan.PrintString("ERROR: the saved activity runtime state did not restore");
		activityStarted = -1;
	}
	if (restoresSnapshot && activityStarted >= 0 && !m_PendingCheckpoint.worldStructure.empty() && !g_MovableMan.LoadWorldStructure(m_PendingCheckpoint.worldStructure)) {
		g_ConsoleMan.PrintString("ERROR: the saved world membership did not restore"); activityStarted = -1;
	}
	if (restoresSnapshot && activityStarted >= 0 && !PrepareCheckpointPrimitives(m_PendingCheckpoint.runtimeGlobals)) {
		g_ConsoleMan.PrintString("ERROR: the saved drawing primitives did not restore"); activityStarted = -1;
	}
	if (restoresSnapshot && activityStarted >= 0 && !m_PendingCheckpoint.scriptGraphs.empty()) {
		g_MovableMan.RestoreLockstepJoinQuarantine(m_PendingCheckpoint.joinQuarantine);
		std::string error;
		if (!g_MovableMan.RestoreScriptGraphs(m_PendingCheckpoint.scriptGraphs, &error)) {
			g_ConsoleMan.PrintString("ERROR: the saved script state did not restore: " + error);
			std::cout << "[scriptgraph] restore failed: " << error << std::endl;
			activityStarted = -1;
		}
	}
	g_MovableMan.SetRestoringSnapshot(false);
	if (restoresSnapshot) {
		g_SceneMan.SetSceneToLoad(m_PendingCheckpoint.restartPreset, m_PendingCheckpoint.restartObjects, m_PendingCheckpoint.restartUnits);

	}
	if (restoresSnapshot && activityStarted >= 0) {
		g_MovableMan.ResolvePendingSnapshotLinks();
		g_MovableMan.RedrawRestoredMOIDs();
		g_MovableMan.ReapplyPersistedControllerModes();
		if (!m_Activity->ResolveCheckpointReferences() || !g_PrimitiveMan.ResolveCheckpointReferences()) {
			g_ConsoleMan.PrintString("ERROR: the saved runtime references or globals did not restore");
			activityStarted = -1;
		}

		if (m_PendingCheckpoint.uniqueIDCounter >= 0) {
			MovableObject::PinUniqueIDCounter(m_PendingCheckpoint.uniqueIDCounter);
		}
		if (m_PendingCheckpoint.luaStateCursor >= 0) {
			g_LuaMan.SetScriptStateCursor(m_PendingCheckpoint.luaStateCursor);
		}
	}
	if (!restoresSnapshot) g_TimerMan.PauseSim(false);

	std::cout << "[snapbench] restart_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - restartStart).count() << std::endl;
	if (activityStarted >= 0) {
		m_InActivity = true;
		return true;
	} else {
		if (restoresSnapshot) return false;
		m_InActivity = false;
		PauseActivity();
		g_ConsoleMan.SetEnabled(true);
		return false;
	}
}

void ActivityMan::EndActivity() const {
	// TODO: Set the activity pointer to nullptr so it doesn't return junk after being destructed. Do it here, or wherever works without crashing.
	if (m_Activity) {
		g_ThreadMan.GetPriorityThreadPool().wait_for_tasks();
		g_ThreadMan.GetBackgroundThreadPool().wait_for_tasks();

		m_Activity->End();
		g_ConsoleMan.PrintString("SYSTEM: Activity \"" + m_Activity->GetPresetName() + "\" was ended");
	} else {
		g_ConsoleMan.PrintString("ERROR: No Activity to end!");
	}
}

void ActivityMan::LateUpdateGlobalScripts() const {
	if (GAScripted* scriptedActivity = dynamic_cast<GAScripted*>(m_Activity.get())) {
		static const uint64_t soundPhase = Hash("LateGlobalScripts");
		SoundSimulationScope sounds(0, soundPhase);
		scriptedActivity->UpdateGlobalScripts(true);
	}
}

void ActivityMan::Update() {
	g_PerformanceMan.StartPerformanceMeasurement(PerformanceMan::ActivityUpdate);
	if (m_Activity) {
		static const uint64_t soundPhase = Hash("Activity");
		SoundSimulationScope sounds(0, soundPhase);
		m_Activity->Update();
	}
	g_PerformanceMan.StopPerformanceMeasurement(PerformanceMan::ActivityUpdate);
}

void ActivityMan::RenderUpdate() {
	if (m_Activity) {
		m_Activity->RenderUpdate();
	}
}

std::string ActivityMan::CaptureRuntimeGlobals() const {
	CheckpointWriter writer("RuntimeGlobals9");
	writer(g_SimRNG.SerializeCheckpoint(), g_RenderRNG.SerializeCheckpoint(), g_TimerMan,
		g_MovableMan, g_SceneMan, g_CameraMan, g_FrameMan);
	writer(m_DefaultActivityType, m_DefaultActivityName, m_InActivity, m_ActivityNeedsRestart, m_ActivityNeedsResume,
		m_ResumingActivityFromPauseMenu, m_SkipPauseMenuWhenPausingActivity, m_StartActivityResumed);
	writer(GUIInput::SaveSharedCheckpoint());
	writer(g_UInputMan.SaveCheckpoint());
	writer(g_PostProcessMan.SaveCheckpoint());
	writer(g_PrimitiveMan.SaveCheckpoint());
	writer(g_GUISound.SaveCheckpoint());
	writer(g_MusicMan.SaveCheckpoint());
	writer(g_AudioMan.SaveCheckpoint());
	return writer.Text();
}

bool ActivityMan::RestoreRuntimeGlobals(std::string_view text, bool validateOnly) {
	try {
		const bool legacy = text.starts_with("15 RuntimeGlobals1 ");
		const bool version2 = text.starts_with("15 RuntimeGlobals2 ");
		const bool version3 = text.starts_with("15 RuntimeGlobals3 ");
		const bool version4 = text.starts_with("15 RuntimeGlobals4 ");
		const bool version5 = text.starts_with("15 RuntimeGlobals5 ");
		const bool version6 = text.starts_with("15 RuntimeGlobals6 ");
		const bool version7 = text.starts_with("15 RuntimeGlobals7 ");
		const bool version8 = text.starts_with("15 RuntimeGlobals8 ");
		CheckpointReader reader(text, legacy ? "RuntimeGlobals1" : version2 ? "RuntimeGlobals2" : version3 ? "RuntimeGlobals3" : version4 ? "RuntimeGlobals4" : version5 ? "RuntimeGlobals5" : version6 ? "RuntimeGlobals6" : version7 ? "RuntimeGlobals7" : version8 ? "RuntimeGlobals8" : "RuntimeGlobals9", validateOnly);
		std::string simState, renderState;
		reader.Value(simState); reader.Value(renderState);
		RandomGenerator sim = g_SimRNG, render = g_RenderRNG;
		if (!sim.RestoreCheckpoint(simState) || !render.RestoreCheckpoint(renderState)) return false;
		const auto component = [&reader](auto& object, const char* name) {
			std::string state; reader.Value(state);
			if (!object.LoadCheckpoint(state, true)) throw std::runtime_error(std::string("invalid ") + name + " checkpoint");
			reader.OnCommit([&object, state, name] { if (!object.LoadCheckpoint(state)) throw std::runtime_error(std::string("could not apply ") + name + " checkpoint"); });
		};
		component(g_TimerMan, "TimerMan"); component(g_MovableMan, "MovableMan");
		component(g_SceneMan, "SceneMan"); component(g_CameraMan, "CameraMan");
		if (!legacy) component(g_FrameMan, "FrameMan");
		if (!legacy && !version2) reader(m_DefaultActivityType, m_DefaultActivityName, m_InActivity, m_ActivityNeedsRestart, m_ActivityNeedsResume,
			m_ResumingActivityFromPauseMenu, m_SkipPauseMenuWhenPausingActivity, m_StartActivityResumed);
		if (!legacy && !version2 && !version3) {
			std::string input; reader.Value(input);
			if (!GUIInput::LoadSharedCheckpoint(input, true)) throw std::runtime_error("invalid shared GUI input checkpoint");
			reader.OnCommit([input] { if (!GUIInput::LoadSharedCheckpoint(input)) throw std::runtime_error("could not restore GUI input checkpoint"); });
		}
		reader.OnCommit([sim, render] { g_SimRNG = sim; g_RenderRNG = render; });
		if (!legacy && !version2 && !version3 && !version4 && !version5) component(g_UInputMan, "UInputMan");
		if (!legacy && !version2 && !version3 && !version4 && !version5) component(g_PostProcessMan, "PostProcessMan");
		if (!legacy && !version2 && !version3 && !version4 && !version5 && !version6) component(g_PrimitiveMan, "PrimitiveMan");
		if (!legacy && !version2 && !version3 && !version4) {
			const bool hasMusic = !version5 && !version6 && !version7;
			const bool hasGUI = hasMusic && !version8;
			std::string gui;
			if (hasGUI) {
				reader.Value(gui);
				if (!g_GUISound.LoadCheckpoint(gui, true)) throw std::runtime_error("invalid GUISound checkpoint");
			}
			std::string music;
			if (hasMusic) {
				reader.Value(music);
				if (!g_MusicMan.LoadCheckpoint(music, true)) throw std::runtime_error("invalid MusicMan checkpoint");
			}
			std::string audio; reader.Value(audio);
			if (!g_AudioMan.LoadCheckpoint(audio, true)) throw std::runtime_error("invalid AudioMan checkpoint");
			reader.OnCommit([audio, music, gui, hasMusic, hasGUI] {
				const bool restored = hasGUI ? g_GUISound.LoadCheckpointWithAudio(gui, music, audio) :
					hasMusic ? g_MusicMan.LoadCheckpointWithAudio(music, audio) : g_AudioMan.LoadCheckpoint(audio);
				if (!restored) throw std::runtime_error("could not restore GUI/music/audio checkpoint");
			});
		}
		reader.Finish();
		return true;
	} catch (const std::exception& error) {
		std::cout << "[runtime-globals] " << (validateOnly ? "validation" : "apply") << " failed: " << error.what() << std::endl;
		return false;
	}
}

bool ActivityMan::PrepareCheckpointMaterials(std::string_view runtimeGlobals) {
	if (runtimeGlobals.empty()) return true;
	try {
		std::string version;
		for (int number = 1; number <= 9; ++number) {
			const std::string candidate = "RuntimeGlobals" + std::to_string(number);
			if (runtimeGlobals.starts_with("15 " + candidate + " ")) { version = candidate; break; }
		}
		if (version.empty()) return false;
		CheckpointReader reader(runtimeGlobals, version, true);
		std::string state;
		// RNGs, TimerMan and MovableMan precede SceneMan in every supported version.
		for (int field = 0; field < 5; ++field) reader.Value(state);
		return g_SceneMan.PrepareCheckpointMaterials(state);
	} catch (const std::exception&) { return false; }
}

bool ActivityMan::PrepareCheckpointPrimitives(std::string_view runtimeGlobals) {
	const bool version7 = runtimeGlobals.starts_with("15 RuntimeGlobals7 ");
	const bool version8 = runtimeGlobals.starts_with("15 RuntimeGlobals8 ");
	const bool version9 = runtimeGlobals.starts_with("15 RuntimeGlobals9 ");
	if (!version7 && !version8 && !version9) return true;
	try {
		CheckpointReader reader(runtimeGlobals, version7 ? "RuntimeGlobals7" : version8 ? "RuntimeGlobals8" : "RuntimeGlobals9", true);
		std::string state;
		for (int field = 0; field < 9; ++field) reader.Value(state); // RNGs, five managers and two default-activity names.
		bool flag;
		for (int field = 0; field < 6; ++field) reader.Value(flag);
		for (int field = 0; field < 4; ++field) reader.Value(state); // Shared input, UInput, postprocessing and primitives.
		return g_PrimitiveMan.LoadCheckpoint(state, false, false);
	} catch (const std::exception&) { return false; }
}

bool ActivityMan::RestartActivity() {
	if (!m_RestartRestoresSnapshot) return RestartActivityCandidate();
	std::string error;
	if (!m_PendingCheckpoint.activity || !m_PendingCheckpoint.scene || !g_MovableMan.ValidateScriptGraphs(m_PendingCheckpoint.scriptGraphs, &error)) {
		g_ConsoleMan.PrintString("ERROR: the saved script state is invalid: " + error);
		m_ActivityNeedsRestart = false;
		return false;
	}
	const std::string oldGlobals = CaptureRuntimeGlobals();
	const std::string oldFrame = g_FrameMan.SaveCheckpoint();
	std::unique_ptr<ContentFile::MemoryPNGScope> committedImages;
	MovableMan::WorldSetAside originalWorld;
	if (!g_MovableMan.SetAsideWorld(originalWorld, false)) { m_ActivityNeedsRestart = false; return false; }
	SceneMan::SceneSetAside originalScene;
	g_SceneMan.SetAsideScene(originalScene);
	auto originalActivity = std::move(m_Activity);
	auto originalStart = std::move(m_StartActivity);
	bool restored = false;
	try {
		Entity::CheckpointCloneScope checkpointClones(true);
		if (!PrepareCheckpointMaterials(m_PendingCheckpoint.runtimeGlobals)) throw std::runtime_error("could not prepare saved Material owners");
		g_AudioMan.ActivateCheckpointSoundRegistrations(m_PendingCheckpoint.soundRegistrations);
		if (m_PendingCheckpoint.images) m_PendingCheckpoint.images->SetActive(true);
		g_SceneMan.SetSceneToLoad(m_PendingCheckpoint.scene.get(), true, true);
		m_StartActivity.reset(dynamic_cast<Activity*>(m_PendingCheckpoint.activity->Clone()));
		m_StartActivityResumed = true;
		restored = RestartActivityCandidate();
		if (restored && m_PendingCheckpoint.hasStartActivity) {
			g_MovableMan.SetRestoringSnapshot(true);
			m_StartActivity.reset(m_PendingCheckpoint.startActivity ? static_cast<Activity*>(m_PendingCheckpoint.startActivity->Clone()) : nullptr);
			if (m_StartActivity) restored = m_StartActivity->ApplyPendingCheckpoint() && m_StartActivity->PrepareCheckpointUI() && m_StartActivity->ResolveCheckpointReferences();
		}
		if (restored && !m_PendingCheckpoint.runtimeGlobals.empty()) restored = RestoreRuntimeGlobals(m_PendingCheckpoint.runtimeGlobals);
		if (restored && m_PendingCheckpoint.uniqueIDCounter >= 0) MovableObject::PinUniqueIDCounter(m_PendingCheckpoint.uniqueIDCounter);
		if (restored && m_PendingCheckpoint.luaStateCursor >= 0) g_LuaMan.SetScriptStateCursor(m_PendingCheckpoint.luaStateCursor);
	} catch (const std::exception& exception) {
		g_ConsoleMan.PrintString(std::string("ERROR: the saved game could not be constructed: ") + exception.what());
	}
	g_MovableMan.SetRestoringSnapshot(false);
	if (restored) {
		g_MovableMan.DiscardWorld(originalWorld);
		committedImages = std::move(m_PendingCheckpoint.images);
		if (committedImages) committedImages->Commit();
		PendingCheckpoint completed = std::move(m_PendingCheckpoint);
		m_PendingCheckpoint = PendingCheckpoint{};
		if (!completed.runtimeGlobals.starts_with("15 RuntimeGlobals5 ") && !completed.runtimeGlobals.starts_with("15 RuntimeGlobals6 ") && !completed.runtimeGlobals.starts_with("15 RuntimeGlobals7 ") && !completed.runtimeGlobals.starts_with("15 RuntimeGlobals8 ") && !completed.runtimeGlobals.starts_with("15 RuntimeGlobals9 ")) {
			g_AudioMan.StopAll();
			g_MusicMan.ResetMusicState();
			g_AudioMan.PauseIngameSounds(m_Activity && m_Activity->IsPaused());
		}
		return true;
	}
	auto rejectedActivity = std::move(m_Activity);
	m_Activity = std::move(originalActivity);
	m_StartActivity = std::move(originalStart);
	g_SceneMan.ReinstateScene(originalScene);
	const bool reinstated = g_MovableMan.ReinstateWorld(originalWorld);
	if (m_PendingCheckpoint.images) m_PendingCheckpoint.images->SetActive(false);
	m_RestartRestoresSnapshot = true;
	g_FrameMan.LoadCheckpoint(oldFrame);
	RestoreRuntimeGlobals(oldGlobals);
	m_ActivityNeedsRestart = false;
	if (!reinstated) g_ConsoleMan.PrintString("ERROR: the prior game's Lua state could not be reinstated");
	return false;
}
